"""Regressions for inventory extraction; these are not protocol execution tests."""
import unittest
from unittest.mock import patch
import generate_inventory as audit


class DispatchInventoryTests(unittest.TestCase):
    def test_same_line_and_multiline_fallthrough_keep_all_callbacks(self):
        source = '''switch (f.serviceType) {
  case msg::FIRST: case msg::SECOND:
  case msg::THIRD:
    if (!f.payload.empty()) { log("bad", "payload"); return; }
    if (host_) host_->onAppEvent(f.serviceType);
    return;
  case msg::FOURTH: handleVideo(f); return;
}'''
        path = audit.REPO_ROOT / audit.SOURCES['carlife_local_session_cpp']
        with patch.object(audit, 'read_lines', return_value=source.splitlines()):
            rows, gaps, meta = audit.collect_dispatch(path)
        cases = {r['name']: r for r in rows if r['category'] == 'carlife.dispatch'}
        self.assertEqual(set(cases), {'FIRST', 'SECOND', 'THIRD', 'FOURTH'})
        for name in ('FIRST', 'SECOND', 'THIRD'):
            self.assertEqual(cases[name]['detail']['host_callback'], ['onAppEvent'])
        self.assertEqual(cases['FIRST']['line'], cases['SECOND']['line'])
        self.assertEqual(cases['FOURTH']['detail']['handler'], 'handleVideo')
        self.assertEqual(meta['msg_cases'], 4)
        self.assertFalse(gaps)

    def test_real_lifecycle_callback_inventory(self):
        rows, _, _ = audit.collect_dispatch(audit.REPO_ROOT / audit.SOURCES['carlife_local_session_cpp'])
        expected = {'GO_TO_DESKTOP', 'SCREEN_ON', 'SCREEN_OFF', 'USER_PRESENT',
                    'FOREGROUND', 'BACKGROUND', 'REQUEST_GO_TO_FOREGROUND', 'GO_TO_FOREGROUND_RESPONSE'}
        found = [r for r in rows if r['category'] == 'carlife.dispatch' and r['name'] in expected]
        self.assertEqual({r['name'] for r in found}, expected)
        self.assertTrue(all('onAppEvent' in r['detail']['host_callback'] for r in found))


if __name__ == '__main__':
    unittest.main()
