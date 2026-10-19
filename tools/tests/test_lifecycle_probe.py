"""Acceptance reports must come from both executed implementations and the current launch."""
import copy
import unittest
import tempfile
import json
from pathlib import Path
from run_lifecycle_probe import validate_report, read_report, profile_features, CLIENT_PROFILES


class LifecycleReportTests(unittest.TestCase):
    def report(self):
        branch = {'passed': True, 'end_play_checked': True,
                  'outcomes': ['Disconnect', 'Connect', 'Cancel'], 'errors': []}
        return {'schema': 2, 'api_contract_version': 1, 'passed': True, 'run_id': 'run', 'pid': 42,
                'editor_build': False, 'with_janus': True, 'network_transport_tested': False,
                'with_direct': True, 'with_media': True, 'with_janus_extensions': True,
                'profile_checked': True, 'janus_blueprint': 'passed',
                'provider': 'DirectUnreal', 'engine_version': '5.8.3-0+UE5', 'error': '', 'cpp': branch, 'blueprint': copy.deepcopy(branch)}

    def test_log_fallback_preserves_one_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            marker = 'LogTemp: SIMURGHRTC_LIFECYCLE_REPORT ' + json.dumps(self.report())
            report, source = read_report(output, marker)
            self.assertEqual(source, 'runtime_log')
            validate_report(report, 'run', 42, False, 'full')
            for log in ('', marker + '\n' + marker):
                with self.assertRaises(ValueError):
                    read_report(output, log)

    def test_accepts_complete_packaged_evidence(self):
        validate_report(self.report(), 'run', 42, False, 'full')

    def test_all_client_profiles_and_excluded_providers(self):
        for profile in CLIENT_PROFILES:
            features, provider = profile_features(profile)
            report = {**self.report(), **features, 'provider': provider,
                      'janus_blueprint': 'passed' if features['with_janus_extensions'] else 'excluded'}
            validate_report(report, 'run', 42, False, profile)
        for profile, provider in [('janus-only', 'DirectUnreal'), ('direct-only', 'Janus'), ('server', None)]:
            with self.assertRaises(ValueError):
                profile_features(profile, provider)

    def test_rejects_missing_profile_checks_and_included_optional_content(self):
        features, provider = profile_features('data-only')
        report = {**self.report(), **features, 'provider': provider, 'janus_blueprint': 'excluded'}
        for key, value in [('profile_checked', False), ('with_media', True), ('with_janus_extensions', True),
                           ('janus_blueprint', 'passed'), ('with_janus', 1), ('schema', 1)]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                validate_report({**report, key: value}, 'run', 42, False, 'data-only')

    def test_rejects_stale_wrong_profile_or_editor_evidence(self):
        for key, value in [('run_id', 'old'), ('pid', 41), ('with_janus', False), ('editor_build', True),
                           ('provider', 'Janus'), ('api_contract_version', 0), ('engine_version', '5.8.2-0'), ('network_transport_tested', True)]:
            with self.subTest(field=key):
                report = self.report(); report[key] = value
                with self.assertRaises(ValueError):
                    validate_report(report, 'run', 42, False, 'full')

    def test_rejects_missing_duplicate_or_failed_branches(self):
        for implementation in ('cpp', 'blueprint'):
            for key, value in [('end_play_checked', False), ('passed', False), ('errors', ['late callback']),
                               ('outcomes', ['Disconnect', 'Connect']),
                               ('outcomes', ['Disconnect', 'Connect', 'Cancel', 'Cancel'])]:
                with self.subTest(implementation=implementation, field=key):
                    report = self.report(); report[implementation][key] = value
                    with self.assertRaises(ValueError):
                        validate_report(report, 'run', 42, False, 'full')


if __name__ == '__main__':
    unittest.main()
