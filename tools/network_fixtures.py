"""Scoped HTTPS and TURN fixtures for the plugin's packaged network probes."""
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import ipaddress
import json
import os
from pathlib import Path
import secrets
import signal
import socket
import ssl
import subprocess
import threading
import time
import mobile_artifacts as a
from run_direct_probe import PATH


def certificates(folder):
    folder.mkdir(parents=True, exist_ok=False);folder.chmod(0o700)
    openssl='/opt/homebrew/opt/openssl@3/bin/openssl'
    def run(*args):subprocess.run([openssl,*map(str,args)],check=True,capture_output=True)
    ca=folder/'ca.pem';key=folder/'ca.key'
    run('req','-x509','-newkey','ec','-pkeyopt','ec_paramgen_curve:prime256v1','-nodes','-days','2',
        '-subj','/CN=SimurghRTC local probe CA','-addext','basicConstraints=critical,CA:TRUE',
        '-addext','keyUsage=critical,keyCertSign,cRLSign','-keyout',key,'-out',ca)
    for kind in ('trusted','wrong-host','untrusted'):
        leaf_key=folder/(kind+'.key');csr=folder/(kind+'.csr');cert=folder/(kind+'.pem')
        run('req','-new','-newkey','ec','-pkeyopt','ec_paramgen_curve:prime256v1','-nodes',
            '-subj','/CN=SimurghRTC local probe','-keyout',leaf_key,'-out',csr)
        ext=folder/(kind+'.ext');ext.write_text('basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\nsubjectAltName='+('DNS:wrong.invalid' if kind=='wrong-host' else 'IP:127.0.0.1,DNS:localhost')+'\n')
        authority=['-signkey',leaf_key] if kind=='untrusted' else ['-CA',ca,'-CAkey',key,'-CAcreateserial']
        run('x509','-req','-in',csr,*authority,'-days','2','-extfile',ext,'-out',cert)
    for path in folder.iterdir():path.chmod(0o600)
    return {p.name:a.digest(p) for p in folder.glob('*.pem')}


class HttpsProxy:
    def __init__(self, upstream, cert, key):
        self.upstream=upstream;self.requests=0;self.tls_failures=0;self.statuses=[];self.tls_versions=set()
        self.outage=threading.Event();self.dropped_requests=0
        self.hold_release=threading.Event();self.hold_release.set();self.held_requests=0
        context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.minimum_version=ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(cert,key);outer=self
        class Server(ThreadingHTTPServer):
            daemon_threads=True
            def get_request(self):
                connection,address=super().get_request();connection.settimeout(5)
                try:
                    secured=context.wrap_socket(connection,server_side=True)
                    outer.tls_versions.add(secured.version());return secured,address
                except (ssl.SSLError,OSError):
                    outer.tls_failures+=1;connection.close();raise
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def do_POST(self):
                if self.path!=PATH:self.send_error(404);return
                try:length=int(self.headers.get('Content-Length','-1'))
                except ValueError:length=-1
                if not 0<=length<=262144:self.send_error(413);return
                body=self.rfile.read(length)
                if len(body)!=length:self.send_error(400);return
                if not outer.hold_release.is_set():
                    outer.held_requests+=1
                    outer.hold_release.wait(30)
                    # Discard the interrupted request even after release. A resumed
                    # sample must send a fresh create request for a fresh session.
                    self.close_connection=True
                    try:self.connection.shutdown(socket.SHUT_RDWR)
                    except OSError:pass
                    return
                if outer.outage.is_set():
                    outer.dropped_requests+=1
                    self.close_connection=True
                    self.connection.shutdown(socket.SHUT_RDWR)
                    return
                remote=http.client.HTTPConnection('127.0.0.1',outer.upstream,timeout=5)
                try:
                    remote.request('POST',PATH,body,{'Content-Type':'application/json','Authorization':self.headers.get('Authorization','')})
                    response=remote.getresponse();data=response.read(262145)
                    if len(data)>262144:raise ValueError('Oversized signaling response')
                    outer.requests+=1;outer.statuses.append(response.status)
                    self.send_response(response.status);self.send_header('Content-Type','application/json')
                    self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
                except (OSError,http.client.HTTPException,ValueError):self.send_error(502)
                finally:remote.close()
        self.server=Server(('127.0.0.1',0),Handler);self.port=self.server.server_port
        self.thread=threading.Thread(target=self.server.serve_forever,daemon=True)
    def __enter__(self):self.thread.start();return self
    def __exit__(self,*args):self.hold_release.set();self.server.shutdown();self.server.server_close();self.thread.join(timeout=5)
    def set_hold(self, enabled):
        if enabled:self.hold_release.clear()
        else:self.hold_release.set()
    def set_outage(self, enabled):
        if enabled:self.outage.set()
        else:self.outage.clear()
    def report(self):return dict(requests=self.requests,dropped_requests=self.dropped_requests,held_requests=self.held_requests,tls_failures=self.tls_failures,statuses=self.statuses,tls_versions=sorted(self.tls_versions))


class TurnFixture:
    """Local test TURN with short-lived, private credentials; no system service changes."""
    def __init__(self, manifest, folder, listen_ip='127.0.0.1'):
        self.identity=a.read_json(manifest);self.executable=Path(self.identity['executable'])
        a.require(a.digest(self.executable)==self.identity['sha256'],'TURN executable changed.')
        a.require(ipaddress.ip_address(listen_ip).version==4,'Select an explicit local IPv4 address.')
        self.listen_ip=listen_ip;self.folder=folder;folder.mkdir(parents=True,exist_ok=False);folder.chmod(0o700)
        self.username='probe-'+secrets.token_hex(8);self.credential=secrets.token_urlsafe(32)
        with socket.socket() as s:s.bind((listen_ip,0));self.port=s.getsockname()[1]
        # Relay traffic stays on this Mac; both peers must use TURN in this fixture.
        # This deliberately does not represent a public relay deployment.
        self.config=folder/'turnserver.conf'
        text='\n'.join(['listening-ip='+listen_ip,'relay-ip=127.0.0.1','listening-port='+str(self.port),
            'realm=simurghrtc.fixture.invalid','fingerprint','lt-cred-mech','user='+self.username+':'+self.credential,
            'min-port=49160','max-port=49191','allow-loopback-peers','no-multicast-peers','no-cli','no-tls','no-dtls',
            'no-tcp-relay','total-quota=8','user-quota=8','max-allocate-lifetime=60','relay-threads=1',
            'log-file=stdout','simple-log','pidfile='+str(folder/'turn.pid')])+'\n'
        with self.config.open('x') as stream:os.chmod(self.config,0o600);stream.write(text)
        self.process=None;self.log=None;self.paused=False
    def config_for(self,host=None,transport='udp',bad_credential=False):
        a.require(transport in ('udp','tcp'),'Unsupported TURN probe transport.')
        return {'relay_only':True,'servers':[{'urls':['turn:'+(host or self.listen_ip)+':'+str(self.port)+'?transport='+transport],
                'username':self.username,'credential':self.credential+('-wrong' if bad_credential else '')}]}
    def __enter__(self):
        self.log=(self.folder/'turn.log').open('w')
        self.process=subprocess.Popen([self.executable,'-c',self.config],stdout=self.log,stderr=subprocess.STDOUT)
        until=time.monotonic()+10
        while time.monotonic()<until and self.process.poll() is None:
            try:
                with socket.create_connection((self.listen_ip,self.port),timeout=.2):return self
            except OSError:time.sleep(.1)
        self.__exit__();raise ValueError('Local TURN fixture did not start.')
    def pause(self):
        a.require(self.process is not None and self.process.poll() is None and not self.paused,'TURN fixture is not running.')
        self.process.send_signal(signal.SIGSTOP);self.paused=True
    def resume(self):
        if self.paused:
            a.require(self.process is not None and self.process.poll() is None,'Paused TURN fixture exited.')
            self.process.send_signal(signal.SIGCONT);self.paused=False
    def __exit__(self,*args):
        if self.process and self.process.poll() is None:
            self.resume()
            self.process.terminate()
            try:self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:self.process.kill();self.process.wait(timeout=5)
        if self.log:self.log.close()
        self.config.unlink(missing_ok=True)
