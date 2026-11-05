"""WeRead shelf covers never hold the reader hostage: quiet-time start, input aborts."""
from pathlib import Path
import unittest

from test_reading_ui_regressions import method

ROOT = Path(__file__).resolve().parents[2]


def read(rel):
    return (ROOT / rel).read_text()


class WeReadResponsiveShelfTest(unittest.TestCase):
    def test_http_waits_ask_the_abort_probe(self):
        client = read('lib/WeReadWebApi/src/WeReadHttpClient.cpp')
        self.assertIn('if (now - g_lastAbortCheck < 15) return false;', client)
        # Every place a request waits on the network asks.
        self.assertGreaterEqual(client.count('if (abortRequested()) return'), 4)
        request = client[client.index('Result request(Session& session'):]
        self.assertLess(request.index('if (abortRequested()) {'), request.index('runRequest('))
        self.assertIn('result = Result::Aborted;', request)
        self.assertIn('class AbortScope', read('lib/WeReadWebApi/src/WeReadHttpClient.h'))

    def test_shelf_covers_wait_for_quiet_and_give_way_to_input(self):
        activity = read('src/activities/apps/weread/webapi/WeReadActivity.cpp')
        advance = method(activity, 'void WeReadActivity::advanceShelfCovers()')
        self.assertIn('WeReadHttpClient::AbortScope abortOnInput(&WeReadActivity::coverAbortProbe, this);', advance)
        self.assertIn('if (millis() - shelfQuietSince_ < kShelfCoverQuietMs) return;', advance)
        self.assertNotIn('requestUpdate();  // a pass', advance)  # no redraw while waiting
        interrupted = advance[advance.index('if (coverInterrupted_) {'):]
        self.assertLess(interrupted.index('operation_.cancel();'), interrupted.index('operation_.reset();'))
        self.assertNotIn('shelfCoverStopped_ = true', interrupted[:interrupted.index('return;')])
        probe = method(activity, 'bool WeReadActivity::coverAbortProbe(void* context)')
        self.assertIn('self->pendingBack_ = true;', probe)
        self.assertIn('self->pendingHome_ = true;', probe)
        main = method(activity, 'void WeReadActivity::handleMainInput()')
        self.assertIn('if (pendingBack_ || mappedInput.wasReleased(MappedInputManager::Button::Back)) {', main)
        self.assertIn('activityManager.goHome();', main[:main.index('pendingBack_ ||')])


if __name__ == '__main__':
    unittest.main()
