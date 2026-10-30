#!/usr/bin/env python3
"""Narrow static contracts for isolated Settings raw diagnostic; not native QA."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
WINDOW = (ROOT / 'src/platform/windows/window.cpp').read_text()
CMAKE = (ROOT / 'CMakeLists.txt').read_text()
MAIN = (ROOT / 'src/app/windows_main.cpp').read_text()
MACRO = 'STORMTTY_QUALIFICATION_SETTINGS_RAW_BOUNDARY'


class SettingsRawSourceContracts(unittest.TestCase):
    def test_only_keyboard_messages_are_consumed_by_key_guard(self):
        body = WINDOW.split('bool NativeWindow::key_message(MSG& message) {', 1)[1].split('const auto vk', 1)[0]
        nonkey = body.index('if (!down && !up) return false;')
        guard = body.index('if(options_.smoke_settings_raw)return true;')
        self.assertLess(nonkey, guard)
        self.assertNotIn('return true', body[:nonkey])
        self.assertIn('#ifdef ' + MACRO, body[nonkey:guard])

    def test_paste_has_separate_dispatch_and_same_retained_actor(self):
        progress = WINDOW.split('void NativeWindow::settings_raw_progress(bool presented)', 1)[1].split('void NativeWindow::smoke_progress', 1)[0]
        self.assertIn('settings_panel_.cancel();settings_input_changed();', progress)
        self.assertIn('if(!presented||settings_panel_.is_open()||motion_timer_active_)return;', progress)
        self.assertIn('PostMessageW(hwnd_,smoke_settings_input_message,0,0)', progress)
        self.assertNotIn('actor->paste', progress)
        dispatch = WINDOW.split('case smoke_settings_input_message:', 1)[1].split('#endif', 1)[0]
        self.assertIn('#ifdef ' + MACRO, dispatch)
        self.assertIn('settings_raw_stage_==3', dispatch)
        self.assertIn('active_session()!=smoke_settings_actor_.get()', dispatch)
        self.assertIn('found->second!=smoke_settings_actor_', dispatch)
        self.assertIn('const auto actor=smoke_settings_actor_', dispatch)
        self.assertLess(dispatch.index('snapshot().completed'), dispatch.index('actor->paste(payload)'))
        self.assertEqual(dispatch.count('actor->paste(payload)'), 1)
        self.assertEqual(dispatch.count('actor->key(enter)'), 1)

    def test_exact_one_real_resize_then_acknowledgment_before_post(self):
        progress = WINDOW.split('void NativeWindow::settings_raw_progress(bool presented)', 1)[1].split('void NativeWindow::smoke_progress', 1)[0]
        self.assertEqual(progress.count('SetWindowPos('), 1)
        self.assertLess(progress.index('settings_panel_.cancel()'), progress.index('SetWindowPos('))
        self.assertLess(progress.index('settings_raw_stage_=2'), progress.index('SetWindowPos('))
        self.assertIn('window.right-window.left+80', progress)
        self.assertIn('settings_raw_client_after_!=settings_raw_client_before_+80', progress)
        gate = progress.split('if(settings_raw_stage_==2)', 1)[1].split('PostMessageW(', 1)[0]
        for check in ('control.requested_resize_generation<=settings_raw_resize_before_',
                      'control.applied_resize_generation!=control.requested_resize_generation',
                      '*control.applied_grid!=control.requested_grid',
                      'control.applied_grid->columns<=settings_raw_columns_before_'):
            self.assertIn(check, gate)
        self.assertIn('settings_raw_target_grid_=*control.applied_grid', gate)
        dispatch = WINDOW.split('case smoke_settings_input_message:', 1)[1].split('#endif', 1)[0]
        before_paste = dispatch.split('actor->paste(payload)', 1)[0]
        for check in ('d.windows_control->requested_resize_generation!=settings_raw_resize_applied_',
                      'd.windows_control->applied_resize_generation!=settings_raw_resize_applied_',
                      '*d.windows_control->applied_grid!=settings_raw_target_grid_',
                      'd.windows_control->requested_grid!=settings_raw_target_grid_'):
            self.assertIn(check, before_paste)
        self.assertNotIn('SetWindowPos(', dispatch)
        self.assertIn('options_.smoke_settings_raw && settings_raw_stage_<=2', WINDOW)

    def test_diagnostic_is_not_default_or_shipping_compile_definition(self):
        option = next(line for line in CMAKE.splitlines() if line.startswith('option(STORMTTY_TEST_SETTINGS_RAW_BOUNDARY '))
        self.assertTrue(option.endswith(' OFF)'))
        definitions = [line for line in CMAKE.splitlines() if MACRO in line]
        self.assertEqual(len(definitions), 1)
        self.assertIn('target_compile_definitions(stormtty_settings_raw_probe PRIVATE', definitions[0])
        self.assertNotIn('install(TARGETS stormtty_settings_raw_probe', CMAKE)
        cli = MAIN.split('else if (argument == L"--smoke-settings-raw")', 1)[0]
        self.assertGreater(cli.rfind('#ifdef ' + MACRO), cli.rfind('#endif'))
        self.assertIn('This isolated diagnostic requires --smoke-settings-raw; normal sessions are disabled.', MAIN)


if __name__ == '__main__':
    unittest.main()
