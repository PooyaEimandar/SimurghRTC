"""Reject excluded code, reflected types and duplicate native dependencies in real map format."""
import unittest
from audit_profile_package import check_modules


class ProfilePackageTests(unittest.TestCase):
    def inputs(self):
        objects = '\n'.join(f'[1] /host/Development/{name}/Module.{name}.cpp.o'
                            for name in ('SimurghRTCCore', 'SimurghRTCNative', 'SimurghRTCDirect', 'SimurghRTCJanus'))
        objects += '\n[2] ThirdParty/WebRTC/5414/Lib/Mac/Release/libwebrtc.a(factory.o)'
        return objects, {'Modules': [{'Name': 'SimurghRTCCore'}, {'Name': 'ContractHost'}]}

    def test_data_only_keeps_both_providers_without_media_or_extensions(self):
        objects, uht = self.inputs()
        self.assertEqual(len(check_modules(objects, uht, 'data-only')), 4)

    def test_rejects_wrong_modules_reflection_and_editor_code(self):
        objects, uht = self.inputs()
        for extra in ('SimurghRTCMedia', 'SimurghRTCJanusExtensions', 'SimurghRTCServer', 'ContractHostEditor'):
            with self.subTest(extra=extra), self.assertRaises(ValueError):
                check_modules(objects + f'\n[3] /host/Development/{extra}/Module.{extra}.cpp.o', uht, 'data-only')
        with self.assertRaises(ValueError):
            check_modules(objects, {'Modules': uht['Modules'] + [{'Name': 'SimurghRTCMedia'}]}, 'data-only')
        with self.assertRaises(ValueError):
            check_modules(objects.replace('SimurghRTCJanus', 'OtherModule'), uht, 'data-only')

    def test_rejects_missing_or_duplicate_webrtc_archives(self):
        objects, uht = self.inputs()
        with self.assertRaises(ValueError):
            check_modules(objects.split('\n[2]')[0], uht, 'data-only')
        with self.assertRaises(ValueError):
            check_modules(objects + '\n[3] ThirdParty/WebRTC/Other/libwebrtc.a(factory.o)', uht, 'data-only')


if __name__ == '__main__':
    unittest.main()
