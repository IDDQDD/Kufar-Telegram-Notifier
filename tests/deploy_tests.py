"""Offline deployment checks; Docker and sleep are replaced inside a temporary project."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class DeployTests(unittest.TestCase):
    def setUp(self):
        self.source = Path(__file__).resolve().parents[1]
        self.temp = tempfile.TemporaryDirectory(prefix="kufar-deploy-test-")
        self.project = Path(self.temp.name).resolve()
        assert self.project.parent == Path(tempfile.gettempdir()).resolve()
        for name in ("deploy.sh", "update.sh", "compose.yaml", "Dockerfile", ".env.example", "kufar-configuration.json", "kufar-proxies.txt"):
            shutil.copyfile(self.source / name, self.project / name)
        self.mock = self.project / "mock-bin"
        self.mock.mkdir()
        (self.mock / "docker").write_text('''#!/usr/bin/env bash
printf '%s\\n' "$*" >> calls.log
case "$*" in
    'compose config --quiet') [[ "${MOCK_FAILURE:-}" != config ]] ;;
    'compose build') [[ "${MOCK_FAILURE:-}" != build ]] ;;
    'compose run --rm --no-deps bot --clear-my-queries') [[ "${MOCK_FAILURE:-}" != clear ]] ;;
    'compose exec -T bot /app/Kufar-Telegram-Notifier --check-kufar') [[ "${MOCK_FAILURE:-}" != api ]] ;;
    'compose ps -aq bot') printf 'test-container\\n' ;;
    inspect*)
        case "${MOCK_FAILURE:-}" in
            restart) printf 'restarting 1\\n' ;;
            past-restart) printf 'running 3\\n' ;;
            *) printf 'running 0\\n' ;;
        esac ;;
    *) exit 0 ;;
esac
''', encoding="utf-8", newline="\n")
        (self.mock / "sleep").write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
        for entry in self.mock.iterdir():
            entry.chmod(0o700)
        self.env = dict(os.environ, PATH=str(self.mock) + os.pathsep + os.environ["PATH"])
        self.bash = os.environ.get("BASH_EXECUTABLE") or shutil.which("bash")
        if not self.bash:
            self.skipTest("Bash is required")

    def tearDown(self):
        self.temp.cleanup()

    def run_deploy(self, *arguments, failure=""):
        return subprocess.run(
            [self.bash, "deploy.sh", *arguments], cwd=self.project,
            env=dict(self.env, MOCK_FAILURE=failure), capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=30,
        )

    def prepare(self):
        result = self.run_deploy("--prepare")
        self.assertEqual(result.returncode, 0, result.stderr)

    def run_update(self, *arguments):
        return subprocess.run(
            [self.bash, "update.sh", *arguments], cwd=self.project,
            env=self.env, capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=30,
        )

    def test_netherlands_update_replaces_primary_and_overrides_preserving_other_settings(self):
        self.prepare()
        other = 'TELEGRAM_BOT_TOKEN="offline-test-secret"\nKEEP_SETTINGS=yes\n'
        original = (other + "KUFAR_PROXY='socks4://86.57.178.182:4153'\n"
                    "KUFAR_PROXY_POOL=http://178.124.195.80:8080\n"
                    "export KUFAR_PROXY_POOL_FILE = '/data/old-proxies.txt'\n"
                    "KUFAR_PROXY=http://178.124.91.214:8080\n")
        (self.project / ".env").write_text(original, encoding="utf-8")
        cache = self.project / "data" / "cached-data.json"
        cache.write_text('{"keep":"history"}', encoding="utf-8")
        result = self.run_update("--netherlands-proxies")
        self.assertEqual(result.returncode, 0, result.stderr)
        updated = (self.project / ".env").read_text()
        self.assertTrue(updated.startswith(other))
        self.assertEqual(updated.count("KUFAR_PROXY="), 1)
        self.assertIn("KUFAR_PROXY=http://85.209.156.148:1080\n", updated)
        self.assertNotIn("KUFAR_PROXY_POOL", updated)
        self.assertNotIn("178.124", updated)
        self.assertNotIn("86.57", updated)
        self.assertNotIn("offline-test-secret", result.stdout + result.stderr)
        self.assertEqual(cache.read_text(), '{"keep":"history"}')
        backups = list(self.project.glob(".env.before-netherlands.*"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_text(), original)
        self.assertFalse(list(self.project.glob(".env.netherlands.*")))

    def test_normal_update_preserves_existing_proxy_settings(self):
        self.prepare()
        original = "KUFAR_PROXY=http://private-proxy:8080\nKUFAR_PROXY_POOL=\n"
        (self.project / ".env").write_text(original, encoding="utf-8")
        result = self.run_update()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.project / ".env").read_text(), original)
        self.assertFalse(list(self.project.glob(".env.before-netherlands.*")))

    def test_netherlands_update_rejects_missing_env_or_extra_arguments(self):
        result = self.run_update("--netherlands-proxies")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.project / ".env").exists())
        self.prepare()
        original = (self.project / ".env").read_bytes()
        result = self.run_update("--netherlands-proxies", "--unknown")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.project / ".env").read_bytes(), original)

    def test_prepare_preserves_settings_and_cache(self):
        self.prepare()
        self.assertFalse((self.project / "calls.log").exists())
        (self.project / ".env").write_text("KEEP_SETTINGS=yes\n", encoding="utf-8")
        cache = self.project / "data" / "cached-data.json"
        cache.write_text('{"keep":"history"}', encoding="utf-8")
        self.prepare()
        self.assertEqual((self.project / ".env").read_text(), "KEEP_SETTINGS=yes\n")
        self.assertEqual(cache.read_text(), '{"keep":"history"}')

    def test_failed_validation_or_build_never_replaces_container(self):
        self.prepare()
        for failure in ("config", "build"):
            with self.subTest(failure=failure):
                (self.project / "calls.log").write_text("")
                result = self.run_deploy(failure=failure)
                self.assertNotEqual(result.returncode, 0)
                calls = (self.project / "calls.log").read_text()
                self.assertNotIn("compose up", calls)
                if failure == "config":
                    self.assertNotIn("compose build", calls)

    def test_start_and_detect_restart_loop(self):
        self.prepare()
        result = self.run_deploy()
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = (self.project / "calls.log").read_text()
        self.assertLess(calls.index("compose build"), calls.index("compose up -d --no-build"))
        self.assertNotEqual(self.run_deploy(failure="restart").returncode, 0)
        self.assertEqual(self.run_deploy(failure="past-restart").returncode, 0,
                         "a historic restart must not fail deployment of a currently healthy container")

    def test_invalid_argument_is_rejected_before_setup(self):
        self.assertNotEqual(self.run_deploy("--unknown").returncode, 0)
        self.assertFalse((self.project / ".env").exists())

    def test_unavailable_search_is_not_reported_as_success(self):
        self.prepare()
        result = self.run_deploy(failure="api")
        self.assertEqual(result.returncode, 2)
        self.assertIn("Поиск Kufar недоступен", result.stderr)
        calls = (self.project / "calls.log").read_text()
        self.assertLess(calls.index("compose up -d --no-build"), calls.index("compose exec -T bot"))
        self.assertNotIn("compose stop", calls)

    def test_clear_owner_queries_builds_before_stopping_and_starts_only_after_clear(self):
        self.prepare()
        result = self.run_deploy("--clear-my-queries")
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = (self.project / "calls.log").read_text()
        self.assertLess(calls.index("compose build"), calls.index("compose stop bot"))
        self.assertLess(calls.index("compose stop bot"), calls.index("compose run --rm --no-deps bot --clear-my-queries"))
        self.assertLess(calls.index("compose run --rm --no-deps bot --clear-my-queries"), calls.index("compose up -d --no-build"))
        (self.project / "calls.log").write_text("")
        result = self.run_deploy("--clear-my-queries", failure="clear")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("compose up", (self.project / "calls.log").read_text())


if __name__ == "__main__":
    unittest.main()
