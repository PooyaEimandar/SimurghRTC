#!/usr/bin/env python3
"""Run the cooked C++/Blueprint data maps on physical Android or an iOS simulator."""
from __future__ import annotations
import argparse
import contextlib
import http.client
import json
import os
from pathlib import Path
import plistlib
import re
import secrets
import shlex
import socket
import subprocess
import time
import uuid
import mobile_artifacts as a
import ios_simulator_support
from run_data_example_probe import MAPS, SCENARIOS, validate_example
from run_direct_probe import PATH, FaultProxy, request, wait_until, validate_server
from run_mobile_probe import (android_main_pid, android_package_sizes, ios_simulator,
                              ios_simulator_runtime, process_alive, read_report, simulator_launch_pid)
from run_signaling_probe import server_bundle_id


class MobileClient:
    def __init__(self, project, app, target, device, output, adb, aapt):
        self.project, self.app, self.target, self.device, self.output = project, app, target, device, output
        self.adb = [str(adb), '-s', device]
        self.config = a.read_json(project/'mobile-probe.json')
        self.app_id = self.config['app_id']
        a.require(re.fullmatch(r'[A-Za-z][A-Za-z0-9]*(\.[A-Za-z][A-Za-z0-9]*){2,}', self.app_id), 'Invalid application ID.')
        self.artifact = a.validate(project/'Plugins/SimurghRTC/ThirdParty/WebRTC'/target/'arm64')
        a.require(self.artifact == self.config['artifacts'][target], 'Staged mobile artifact changed.')
        a.require(self.config['profile'] in ('direct-only', 'full'), 'A Direct-enabled package is required.')
        self.simulator = target == 'IOSSimulator'
        self.paths, self.reverses, self.pid = [], [], None
        self.container = None
        self.identity = dict(platform=target, device=device, profile=self.config['profile'], artifact=self.artifact,
                             scope='simulator' if self.simulator else 'physical-device')
        self.aapt = aapt

    def command(self, args, *, check=True, timeout=45, secret=None):
        args = list(map(str, args))
        result = subprocess.run(args, input=secret, capture_output=True, text=True, timeout=timeout)
        with (self.output/'commands.log').open('a') as stream:
            stream.write(json.dumps(args)+'\n')
            stream.write(('[private file contents omitted]\n' if secret is not None else result.stdout+result.stderr)+'\n')
        a.require(not check or result.returncode == 0, 'Mobile command failed; see commands.log: '+args[0])
        return result

    def install(self):
        if self.simulator:
            support = ios_simulator_support.validate(self.project/'Build/IOSSimulator')
            a.require(support == self.config.get('simulator_support'), 'Simulator support identity changed.')
            with (self.app/'Info.plist').open('rb') as stream: info = plistlib.load(stream)
            a.require(info.get('CFBundleIdentifier') == self.app_id and 'iPhoneSimulator' in info.get('CFBundleSupportedPlatforms', []), 'Wrong simulator package.')
            self.command(['codesign', '--verify', '--deep', '--strict', self.app])
            executable = self.app/info['CFBundleExecutable']
            a.require(self.command(['xcrun', 'lipo', '-archs', executable]).stdout.strip() == 'arm64', 'Expected ARM64 simulator.')
            self.identity.update(executable_sha256=a.digest(executable), simulator_support=support,
                                 size=dict(bundle_file_bytes=sum(p.stat().st_size for p in self.app.rglob('*') if p.is_file()),
                                           executable_bytes=executable.stat().st_size, shipping_baseline=False, rtc_size_delta_measured=False))
            self.identity['device_info'] = ios_simulator(json.loads(self.command(['xcrun','simctl','list','devices','available','--json']).stdout), self.device)
            self.identity['runtime'] = ios_simulator_runtime(json.loads(self.command(['xcrun','simctl','list','runtimes','--json']).stdout), self.identity['device_info']['runtime'])
            self.command(['xcrun','simctl','bootstatus',self.device,'-b'], timeout=180)
            self.command(['xcrun','simctl','install',self.device,self.app], timeout=120)
            self.container = Path(self.command(['xcrun','simctl','get_app_container',self.device,self.app_id,'data']).stdout.strip())
            a.require(self.container.is_dir(), 'Missing simulator application container.')
        else:
            a.require(not self.device.startswith('emulator-') and self.command(self.adb+['get-state']).stdout.strip() == 'device', 'Select an authorized physical Android device.')
            a.require(self.command(self.adb+['shell','getprop','ro.kernel.qemu']).stdout.strip() != '1', 'Physical Android required.')
            a.require('arm64-v8a' in self.command(self.adb+['shell','getprop','ro.product.cpu.abilist']).stdout, 'ARM64 device required.')
            badging = self.command([self.aapt,'dump','badging',self.app]).stdout
            a.require(f"package: name='{self.app_id}'" in badging, 'APK application ID differs.')
            self.identity.update(app_sha256=a.digest(self.app), size=android_package_sizes(self.app),
                                 model=self.command(self.adb+['shell','getprop','ro.product.model']).stdout.strip(),
                                 os=self.command(self.adb+['shell','getprop','ro.build.version.release']).stdout.strip(),
                                 page_size=self.command(self.adb+['shell','getconf','PAGESIZE']).stdout.strip())
            self.command(self.adb+['install','-r',self.app], timeout=120)
            self.command(self.adb+['shell','run-as',self.app_id,'pwd'])  # Development app's private files only.

    def write(self, path, value):
        if self.simulator:
            with Path(path).open('x') as stream:
                os.chmod(path, 0o600); stream.write(value)
        else:
            self.command(self.adb+['shell','run-as',self.app_id,'tee',path], secret=value)
            self.command(self.adb+['shell','run-as',self.app_id,'chmod','600',path])

    def argument_path(self, path):
        # UE's iOS platform file resolves writable relative paths under Library.
        # A simulator's host /Users/... path would be prefixed a second time.
        return str(Path(path).relative_to(self.container/'Library')) if self.simulator else str(path)

    def launch(self, run_id, implementation, scenario, port, token, *, https=False, ice=None, ca=None):
        self.stop()
        if self.simulator:
            folder = self.container/'Library/Caches/SimurghRTC/DataExamples'/run_id
            folder.mkdir(parents=True, exist_ok=False)
        else:
            base = self.command(self.adb+['shell','run-as',self.app_id,'pwd']).stdout.strip()
            a.require(base.startswith('/data/') and base.endswith('/'+self.app_id), 'Unexpected Android private data path.')
            folder = Path(base)/'files/SimurghRTCDataExamples'/run_id
            self.command(self.adb+['shell','run-as',self.app_id,'mkdir','-p',folder])
            reverse = self.command(self.adb+['reverse','tcp:0','tcp:'+str(port)]).stdout.strip()
            a.require(reverse.isdecimal() and 0 < int(reverse) < 65536, 'adb did not allocate a reverse port.')
            self.reverses.append(reverse); port = int(reverse)
        credential, gate = folder/'token', folder/'trigger'
        self.paths += [credential, gate]
        self.write(credential, token)
        options = [MAPS[implementation],'-SimurghRTCDataExampleProbe',
                   '-SimurghRTCExampleRunId='+run_id,'-SimurghRTCExampleImplementation='+implementation,
                   '-SimurghRTCExampleScenario='+scenario,'-SimurghRTCExampleGate='+self.argument_path(gate),
                   '-SimurghRTCExampleEndpoint='+('https' if https else 'http')+'://127.0.0.1:'+str(port)+PATH,
                   '-SimurghRTCExampleTokenFile='+self.argument_path(credential),'-nosound','-unattended']
        if scenario in ('deadline-expire','deadline-release'):
            release=folder/'send-release';self.paths.append(release)
            options += ['-SimurghRTCSendGate='+self.argument_path(release),'-log=send-gate-'+run_id+'.log']
        if not https: options += ['-SimurghRTCExampleLoopback']
        if ice is not None:
            ice_path=folder/'ice.json';self.paths.append(ice_path);self.write(ice_path,json.dumps(ice))
            options += ['-SimurghRTCIceConfigFile='+self.argument_path(ice_path)]
        if ca is not None:
            a.require(not self.simulator, 'Simulator trust belongs to a disposable test simulator.')
            ca_path=folder/'ca.pem';self.paths.append(ca_path);self.write(ca_path,Path(ca).read_text())
            options += ['-ini:Engine:[SSL]:OverrideCertificateBundlePath='+self.argument_path(ca_path)]
        if self.simulator:
            options += ['-nullrhi','-sm5']
            if not ice: options += ['-SimurghRTCSessionLoopbackProbe']
            launch = self.command(['xcrun','simctl','launch','--terminate-running-process',
                                   '--stdout='+str(self.output/(run_id+'-stdout.log')),
                                   '--stderr='+str(self.output/(run_id+'-stderr.log')),self.device,self.app_id,*options])
            self.pid = simulator_launch_pid(launch.stdout, self.app_id)
        else:
            self.command(self.adb+['shell','am','start','-W','-n',self.app_id+'/com.epicgames.unreal.GameActivity','--es','cmdline',shlex.quote(' '.join(options))])
            values = self.command(self.adb+['shell','pidof',self.app_id]).stdout.strip().split()
            a.require(values and all(p.isdecimal() for p in values), 'Android app did not start.')
            processes = self.command(self.adb+['shell','ps','-p',','.join(values),'-o','PID,PPID,NAME']).stdout
            self.pid = android_main_pid(processes,self.app_id)
        return gate

    def report(self, run_id, destination, *, progress=False):
        prefix = 'data-example-progress-' if progress else 'data-example-'
        if self.simulator:
            files = list(self.container.rglob(prefix+run_id+'.json'))
            a.require(len(files) <= 1, 'Duplicate mobile reports.')
            if files:
                try: return json.loads(files[0].read_text())
                except json.JSONDecodeError: pass
        else:
            # Android's UE log sink truncates long JSON lines. Read the complete
            # report from this Development app's own saved directory instead.
            name=prefix+str(uuid.UUID(run_id))+'.json'
            saved='/sdcard/Android/data/'+self.app_id+'/files/UnrealGame/ContractHost/ContractHost/Saved/SimurghRTC/'+name
            data=self.command(self.adb+['shell','run-as',self.app_id,'cat',saved],check=False)
            report=None
            if data.returncode == 0:
                try:report=json.loads(data.stdout)
                except json.JSONDecodeError:pass  # The app may still be writing.
            if progress: return report
            log = self.command(self.adb+['logcat','-d','-v','brief','--pid='+str(self.pid)]).stdout
            (destination/'process.log').write_text(log)
            a.require('Fatal signal ' not in log, 'Android crash; see process.log.')
            return report or read_report(log,run_id,'SIMURGHRTC_DATA_EXAMPLE_REPORT ')
        return None

    def stop(self):
        if self.simulator:
            if self.pid and process_alive(self.pid):
                # A successful probe can exit between the PID check and simctl.
                # The postcondition is process disappearance, including that race.
                self.command(['xcrun','simctl','terminate',self.device,self.app_id], check=False)
                wait_until(lambda:not process_alive(self.pid), 10)
        else:
            self.command(self.adb+['shell','am','force-stop',self.app_id])
        self.pid = None
        for port in self.reverses[:]:
            self.command(self.adb+['reverse','--remove','tcp:'+port]); self.reverses.remove(port)

    def assert_same_process(self):
        if self.simulator:
            a.require(self.pid is not None and process_alive(self.pid),'Simulator sample was terminated during backgrounding.')
        else:
            values=self.command(self.adb+['shell','pidof',self.app_id]).stdout.strip().split()
            a.require(str(self.pid) in values,'Android sample was terminated or restarted during backgrounding.')

    def background(self):
        self.assert_same_process()
        if self.simulator:
            # A real app switch delivers UIKit/Unreal lifecycle callbacks.
            self.command(['xcrun','simctl','launch',self.device,'com.apple.Preferences'])
        else:self.command(self.adb+['shell','input','keyevent','KEYCODE_HOME'])
        self.assert_same_process()

    def foreground(self):
        self.assert_same_process()
        if self.simulator:
            result=self.command(['xcrun','simctl','launch',self.device,self.app_id])
            a.require(simulator_launch_pid(result.stdout,self.app_id)==self.pid,'Resume launched a new simulator process.')
        else:
            self.command(self.adb+['shell','am','start','-W','-n',self.app_id+'/com.epicgames.unreal.GameActivity','-f','0x00020000'])
        self.assert_same_process()

    def cleanup(self):
        self.stop()
        for path in self.paths:
            if self.simulator: Path(path).unlink(missing_ok=True)
            else: self.command(self.adb+['shell','run-as',self.app_id,'rm','-f',path])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('server-project','server-executable','client-project','app','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--platform',choices=['Android','IOSSimulator'],required=True)
    parser.add_argument('--device',required=True)
    parser.add_argument('--adb',type=Path,default=Path.home()/'Library/Android/sdk/platform-tools/adb')
    parser.add_argument('--aapt',type=Path,default=Path.home()/'Library/Android/sdk/build-tools/36.0.0/aapt')
    args = parser.parse_args(); output = args.output.resolve()
    a.require(not output.exists(), 'Use a fresh evidence directory.'); output.mkdir(parents=True)
    client = MobileClient(args.client_project.resolve(),args.app.resolve(),args.platform,args.device,output,args.adb,args.aapt)
    server = args.server_executable.resolve(); project = args.server_project.resolve()
    artifact = a.validate(project/'Plugins/SimurghRTC/ThirdParty/WebRTC/Mac/arm64')
    a.require(a.read_json(project/'Plugins/SimurghRTC/SimurghRTC.profile.json')['profile']=='server','Expected dedicated Server profile.')
    subprocess.run(['codesign','--verify','--deep','--strict',str(server.parents[2])],check=True,capture_output=True)
    a.require(subprocess.check_output(['xcrun','lipo','-archs',server],text=True).strip()=='arm64','Expected ARM64 Server.')
    run_id = str(uuid.uuid4()); token = secrets.token_urlsafe(32)
    folder = Path.home()/'Library/Containers'/server_bundle_id(server)/'Data/Library/Caches/SimurghRTC/MobileData'/run_id
    folder.mkdir(parents=True,exist_ok=False); credential = folder/'token'
    with credential.open('x') as stream: credential.chmod(0o600); stream.write(token)
    result = dict(schema=1,passed=False,run_id=run_id,client=client.identity,clients={},server_artifact=artifact,
                  server_executable_sha256=a.digest(server),signaling='authenticated development HTTP',
                  ice_route='loopback' if client.simulator else 'network interfaces; adb reverse carries HTTP only',
                  https_tested=False,turn_tested=False,physical_ios_tested=False)
    process = None; started = time.monotonic()
    def health():
        status, value = request(port,None,path='/simurghrtc/probe/direct',method='GET')
        a.require(status==200 and value['run_id']==run_id and value['pid']==process.pid,'Wrong Server process.')
        (output/'health-latest.json').write_text(json.dumps(value,indent=2)+'\n'); return value
    def await_health(predicate,timeout=90):
        def poll():
            try:
                value=health();return value if predicate(value) else None
            except (OSError,http.client.HTTPException):return None
        return wait_until(poll,timeout)
    try:
        client.install()
        with socket.socket() as sock: sock.bind(('127.0.0.1',0)); port = sock.getsockname()[1]
        command = [str(server),'-SimurghRTCDirectServerProbe','-SimurghRTCDirectScenario=example',
                   '-SimurghRTCDirectRunId='+run_id,'-SimurghRTCDirectPort='+str(port),
                   '-SimurghRTCDirectTokenFile='+str(credential),'-UserDir='+str(folder),
                   '-nullrhi','-nosound','-unattended','-log=send-gate-'+run_id+'.log']
        if not client.simulator:command += ['-SimurghRTCDirectNetworkProbe']
        (output/'server-command.json').write_text(json.dumps(command,indent=2)+'\n')
        with (output/'server.log').open('w') as stream:process=subprocess.Popen(command,stdout=stream,stderr=subprocess.STDOUT)
        await_health(lambda h:h['running']);identities=[]
        for implementation in MAPS:
            for scenario in SCENARIOS:
                case=implementation+'-'+scenario; dest=output/case;dest.mkdir();cid=str(uuid.uuid4())
                fault=scenario in ('timeout','disconnect-pending','destroy-connect')
                with FaultProxy(port,'timeout') if fault else contextlib.nullcontext() as proxy:
                    gate=client.launch(cid,implementation,scenario,proxy.port if proxy else port,token)
                    pid=client.pid
                    if scenario in ('disconnect-pending','destroy-connect'):
                        wait_until(lambda:proxy.injected,20);client.write(gate,'ready')
                    report=wait_until(lambda:client.report(cid,dest),90)
                    (dest/'probe.json').write_text(json.dumps(report,indent=2)+'\n')
                    identities.append(validate_example(report,cid,pid,client.artifact['manifest_sha256'],client.config['with_janus'],
                        implementation,scenario,platform='IOS' if client.simulator else 'Android',simulator=client.simulator,loopback=client.simulator))
                    if proxy:a.require(proxy.injected,'Connect never reached the HTTP fixture.')
                    result['clients'][case]=dict(run_id=cid,pid=pid,probe=report,http_pending_verified=bool(proxy))
                    client.stop()
                await_health(lambda h:h['live_peers']==0,20)
        a.require(len(set(identities))==10,'Repeated mobile session identity.')
        request(port,token,{},path='/simurghrtc/probe/stop')
        code=process.wait(timeout=30);report=read_report((output/'server.log').read_text(),run_id,'SIMURGHRTC_DIRECT_REPORT ')
        result.update(server=report,server_exit_code=code)
        validate_server(report,run_id,process.pid,artifact['manifest_sha256'],application_messages=True,loopback=client.simulator)
        a.require(code==0 and report['records']==report['echo_pending']==0,'Server cleanup incomplete.')
        a.require(report['created']==report['connected']==report['closed']==4 and report['data_received']==report['data_sent']==4,'Server totals differ.')
        with socket.socket() as sock:a.require(sock.connect_ex(('127.0.0.1',port))!=0,'Server listener remains open.')
        result['passed']=True
    except (ValueError,OSError,KeyError,subprocess.SubprocessError,http.client.HTTPException) as error:result['error']=str(error)
    finally:
        try:client.cleanup();result['client_stopped']=True;result['client_secrets_removed']=True;result['reverse_ports_removed']=True
        except (ValueError,OSError,subprocess.SubprocessError) as error:result.update(passed=False,cleanup_error=str(error))
        if process and process.poll() is None:
            process.terminate()
            try:process.wait(timeout=10)
            except subprocess.TimeoutExpired:process.kill();process.wait(timeout=10)
        credential.unlink(missing_ok=True)
        result.update(server_stopped=process is None or process.poll() is not None,server_secret_removed=not credential.exists(),
                      elapsed_seconds=round(time.monotonic()-started,3))
        (output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(dict(passed=result['passed'],error=result.get('error'),evidence=str(output/'result.json'))))
    return 0 if result['passed'] else 1


if __name__ == '__main__':raise SystemExit(main())
