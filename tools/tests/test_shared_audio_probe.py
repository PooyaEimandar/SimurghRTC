import unittest
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_shared_audio_probe import validate, validate_mixer_log

class SharedAudioReportTests(unittest.TestCase):
    def report(self,server=False):
        r=dict(schema=1,run_id='run',pid=123,server=server,server_build=server,editor_build=False,with_janus=False,
               loopback_only=True,audio_hardware_opened=False,passed=True,error='',phases=4,checks=0 if server else 4,
               pending_cleanup=0,client_native_closed=0 if server else 1,client_remote_closed=0 if server else 1,
               artifact_manifest_sha256='sha',engine_version='5.8.3-test',quiet_ticks=8)
        r['measurements']=[dict(phase=i,samples=72000,correlation=0 if server and i==1 else .99,rms=0 if server and i==1 else .176) for i in range(4)]
        if server:r.update(server_live_peers=0,server_closed=1)
        return r
    def check(self,r,server=False):validate(r,'run',123,'sha',server,False)
    def playback(self):
        r=self.report();r.update(audio_hardware_opened=True,playback_enabled=True,audible_output_tested=False,playback_checks=4)
        r['playback_measurements']=[dict(consumed_samples=96000,callbacks=100,peak_queued_samples=4800,dropped_samples=0,silence_samples=512,rms=.176,callback_on_game_thread=False) for _ in range(3)]
        return r
    def test_accepts_real_mixer_playback(self):
        validate(self.playback(),'run',123,'sha',False,False,True)
    def microphone(self,server=False):
        r=self.report(True) if server else self.playback()
        r['microphone_enabled']=True
        r['measurements'][0].update(correlation=.05,rms=.003)
        r['measurements'][1].update(correlation=0,rms=0)
        if not server:
            r['playback_measurements'][0]['rms']=.002
            r['microphone']=dict(workers=0,opened=1,closed=1,callbacks=900,input_frames=432000,
                                 peak_queued_samples=960,dropped_samples=0,callback_on_game_thread=False)
        return r
    def test_accepts_microphone_echo_with_cleanup(self):
        for server in (False,True):validate(self.microphone(server),'run',123,'sha',server,False,True,True)
    def test_rejects_generated_fixture_as_capture(self):
        with self.assertRaises(ValueError):validate(self.playback(),'run',123,'sha',False,False,True,True)
    def test_rejects_capture_leaks_false_callbacks_and_overflow(self):
        for key,value in [('workers',1),('opened',0),('closed',0),('callback_on_game_thread',True),('callbacks',0),('input_frames',1),('peak_queued_samples',9601),('dropped_samples',-1)]:
            with self.subTest(key=key),self.assertRaises(ValueError):
                r=self.microphone();r['microphone'][key]=value;validate(r,'run',123,'sha',False,False,True,True)
    def test_rejects_silent_microphone_and_unmuted_echo(self):
        for server in (False,True):
            for phase,rms in ((0,0),(1,.01)):
                with self.subTest(server=server,phase=phase),self.assertRaises(ValueError):
                    r=self.microphone(server);r['measurements'][phase]['rms']=rms;validate(r,'run',123,'sha',server,False,True,True)
    def test_requires_mac_mixer_startup(self):
        lines=["Initializing audio mixer using platform API: 'CoreAudio'",'Output buffers initialized: Frames=1024','Starting AudioMixerPlatformInterface::RunInternal()']
        validate_mixer_log('\n'.join(lines))
        for removed in range(3):
            with self.assertRaises(ValueError):validate_mixer_log('\n'.join(line for index,line in enumerate(lines) if index!=removed))
        with self.assertRaises(ValueError):validate_mixer_log('\n'.join(lines).replace('CoreAudio','Null'))
    def test_rejects_headless_report_as_playback(self):
        with self.assertRaises(ValueError):validate(self.report(),'run',123,'sha',False,False,True)
    def test_rejects_incomplete_playback_lifecycle(self):
        for key,value in [('playback_enabled',False),('audible_output_tested',True),('playback_checks',3),('playback_measurements',[])]:
            with self.subTest(key=key),self.assertRaises(ValueError):
                r=self.playback();r[key]=value;validate(r,'run',123,'sha',False,False,True)
    def test_rejects_silent_unbounded_or_game_thread_playback(self):
        for key,value in [('consumed_samples',0),('callbacks',0),('peak_queued_samples',9601),('peak_queued_samples',0),('rms',0),('rms',float('nan')),('callback_on_game_thread',True),('consumed_samples',True)]:
            with self.subTest(key=key),self.assertRaises(ValueError):
                r=self.playback();r['playback_measurements'][1][key]=value;validate(r,'run',123,'sha',False,False,True)
    def test_rejects_missing_report(self):
        with self.assertRaises(ValueError):self.check(None)
    def test_accepts_complete_two_way_measurements(self):
        self.check(self.report());self.check(self.report(True),True)
    def test_rejects_wrong_package_owner_or_cleanup(self):
        for key,value in [('run_id','old'),('pid',124),('artifact_manifest_sha256','stale'),('editor_build',True),('server_build',True),('with_janus',True),('pending_cleanup',1),('client_native_closed',0),('client_remote_closed',0),('quiet_ticks',0),('checks',3)]:
            with self.subTest(key=key),self.assertRaises(ValueError):
                r=self.report();r[key]=value;self.check(r)
    def test_rejects_missing_phase_or_false_signal(self):
        cases=[]
        for key,value in [('samples',0),('correlation',.5),('correlation',float('nan')),('correlation',True),('rms',0),('rms',float('inf')),('phase',2)]:
            r=self.report();r['measurements'][0][key]=value;cases.append(r)
        r=self.report();r['measurements'].pop();cases.append(r)
        for r in cases:
            with self.assertRaises(ValueError):self.check(r)
    def test_rejects_muted_audio_or_live_server_peer(self):
        r=self.report(True);r['measurements'][1]['rms']=.1
        with self.assertRaises(ValueError):self.check(r,True)
        r=self.report(True);r['server_live_peers']=1
        with self.assertRaises(ValueError):self.check(r,True)
