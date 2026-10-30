"""Acceptance gates must reject wrong trust, hostname, routes and error outcomes."""
import http.client
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import ssl
import signal
import sys
import tempfile
import threading
import unittest
from unittest.mock import Mock
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from network_fixtures import certificates, HttpsProxy, TurnFixture
from run_mobile_network_probe import validate_route, validate_tls_rejection
from run_data_example_probe import validate_example
from run_direct_probe import wait_until
import test_data_example_probe as examples


class NetworkFixtureTests(unittest.TestCase):
    def test_turn_cleanup_resumes_only_its_owned_paused_process(self):
        with tempfile.TemporaryDirectory() as temp:
            turn=TurnFixture.__new__(TurnFixture)
            turn.process=Mock();turn.process.poll.return_value=None
            turn.paused=False;turn.log=None;turn.config=Path(temp)/'turn.conf';turn.config.touch()
            turn.pause();turn.__exit__()
            self.assertEqual(turn.process.send_signal.call_args_list, [unittest.mock.call(signal.SIGSTOP),unittest.mock.call(signal.SIGCONT)])
            turn.process.terminate.assert_called_once();self.assertFalse(turn.config.exists());self.assertFalse(turn.paused)
            turn.process.poll.return_value=1
            with self.assertRaises(ValueError):turn.pause()

    def test_requires_selected_relay_pair_and_protocol(self):
        value=dict(local_candidate=4,remote_candidate=4,ice_route_samples=2,relay_protocol=1)
        validate_route(value,'udp');validate_route(dict(value,relay_protocol=2),'tcp')
        for key,bad in [('local_candidate',1),('remote_candidate',1),('ice_route_samples',0),('relay_protocol',2),('relay_protocol',True)]:
            with self.subTest(key=key),self.assertRaises(ValueError):validate_route(dict(value,**{key:bad}),'udp')

    def test_hostname_rejection_can_follow_handshake_but_never_send_http(self):
        validate_tls_rejection(dict(requests=0,tls_failures=3,tls_versions=[]))
        validate_tls_rejection(dict(requests=0,tls_failures=0,tls_versions=['TLSv1.3']))
        for evidence in [dict(requests=1,tls_failures=3),dict(requests=0,tls_failures=0,tls_versions=[])]:
            with self.assertRaises(ValueError):validate_tls_rejection(evidence)

    def test_network_rejections_require_exact_error_and_no_messages(self):
        for scenario,code in [('unauthorized',7),('tls-rejected',11)]:
            value=examples.DataExampleProbeTests().report(scenario='timeout');value.update(scenario=scenario,last_failure=code)
            validate=lambda v:validate_example(v,'run',42,'digest',True,'blueprint',scenario)
            validate(value)
            for key,bad in [('last_failure',8),('received',1),('outcomes',['Connect:Success']),('native_closed',0)]:
                with self.subTest(key=key),self.assertRaises(ValueError):validate(dict(value,**{key:bad}))

    @unittest.skipUnless(Path('/opt/homebrew/opt/openssl@3/bin/openssl').exists(),'Mac fixture OpenSSL required')
    def test_close_race_uses_same_authenticated_session_and_preserves_original_reply(self):
        calls=[]
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def do_POST(self):
                request=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                calls.append((request,self.headers.get('Authorization')))
                response=dict(request,status='accepted' if len(calls)==1 else 'error',
                              body={'state':'closed'} if len(calls)==1 else {'code':'SessionClosed'})
                body=json.dumps(response).encode();self.send_response(200 if len(calls)==1 else 409)
                self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
        upstream=ThreadingHTTPServer(('127.0.0.1',0),Handler)
        thread=threading.Thread(target=upstream.serve_forever,daemon=True);thread.start()
        try:
            with tempfile.TemporaryDirectory() as temp:
                folder=Path(temp)/'certs';certificates(folder)
                with HttpsProxy(upstream.server_port,folder/'trusted.pem',folder/'trusted.key',close_race=True) as proxy:
                    conn=http.client.HTTPSConnection('127.0.0.1',proxy.port,context=ssl.create_default_context(cafile=str(folder/'ca.pem')),timeout=3)
                    original=dict(type='close',session_id='owned-session',request_id='original',sequence=12,body={})
                    try:
                        conn.request('POST','/simurghrtc/v1/signal',json.dumps(original),{'Authorization':'Bearer test-only'})
                        response=conn.getresponse();reply=json.loads(response.read())
                        self.assertEqual(response.status,409);self.assertEqual(reply['body'],{'code':'SessionClosed'})
                        self.assertEqual(reply['request_id'],'original')
                    finally:conn.close()
                    self.assertEqual(proxy.close_races,1);self.assertEqual(proxy.closed_close_responses,1)
                    self.assertEqual(calls[1][0],original)
                    self.assertEqual(calls[0][0]['session_id'],original['session_id'])
                    self.assertNotEqual(calls[0][0]['request_id'],original['request_id'])
                    self.assertEqual([v[1] for v in calls],['Bearer test-only']*2)
        finally:upstream.shutdown();upstream.server_close();thread.join(timeout=3)

    @unittest.skipUnless(Path('/opt/homebrew/opt/openssl@3/bin/openssl').exists(),'Mac fixture OpenSSL required')
    def test_https_verifies_authority_and_hostname_before_forwarding(self):
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def do_POST(self):
                self.rfile.read(int(self.headers['Content-Length']));body=b'{"accepted":true}'
                self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
        upstream=ThreadingHTTPServer(('127.0.0.1',0),Handler)
        thread=threading.Thread(target=upstream.serve_forever,daemon=True);thread.start()
        try:
            with tempfile.TemporaryDirectory() as temp:
                folder=Path(temp)/'certs';hashes=certificates(folder);self.assertEqual(len(hashes),4)
                context=ssl.create_default_context(cafile=str(folder/'ca.pem'))
                for kind in ('trusted','untrusted','wrong-host'):
                    with HttpsProxy(upstream.server_port,folder/(kind+'.pem'),folder/(kind+'.key')) as proxy:
                        conn=http.client.HTTPSConnection('127.0.0.1',proxy.port,context=context,timeout=3)
                        try:
                            if kind=='trusted':
                                conn.request('POST','/simurghrtc/v1/signal','{}');response=conn.getresponse()
                                self.assertEqual(response.status,200);self.assertEqual(response.read(),b'{"accepted":true}');self.assertEqual(proxy.requests,1)
                                conn.close();proxy.set_outage(True)
                                with self.assertRaises(http.client.RemoteDisconnected):
                                    conn.request('POST','/simurghrtc/v1/signal','{}');conn.getresponse()
                                self.assertEqual(proxy.requests,1);self.assertEqual(proxy.dropped_requests,1)
                                conn.close();proxy.set_outage(False)
                                conn.request('POST','/simurghrtc/v1/signal','{}');response=conn.getresponse()
                                self.assertEqual(response.status,200);response.read();self.assertEqual(proxy.requests,2)
                                conn.close();proxy.set_hold(True);errors=[]
                                def held_request():
                                    held=http.client.HTTPSConnection('127.0.0.1',proxy.port,context=context,timeout=5)
                                    try:held.request('POST','/simurghrtc/v1/signal','{}');held.getresponse()
                                    except http.client.RemoteDisconnected:errors.append('discarded')
                                    finally:held.close()
                                blocked=threading.Thread(target=held_request);blocked.start()
                                try:
                                    wait_until(lambda:proxy.held_requests==1,3)
                                    self.assertTrue(blocked.is_alive());self.assertEqual(proxy.requests,2)
                                finally:proxy.set_hold(False);blocked.join(timeout=3)
                                self.assertEqual(errors,['discarded']);self.assertFalse(blocked.is_alive())
                                conn.request('POST','/simurghrtc/v1/signal','{}');response=conn.getresponse();response.read()
                                self.assertEqual(proxy.requests,3)
                            else:
                                with self.assertRaises(ssl.SSLCertVerificationError):conn.request('POST','/simurghrtc/v1/signal','{}')
                                self.assertEqual(proxy.requests,0)
                        finally:conn.close()
                self.assertTrue(all((p.stat().st_mode & 0o777)==0o600 for p in folder.iterdir()))
        finally:upstream.shutdown();upstream.server_close();thread.join(timeout=3)


if __name__=='__main__':unittest.main()
