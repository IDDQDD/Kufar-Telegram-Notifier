"""Real, local Git integration: preserve settings while adopting an archive install."""
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest


class SetupGitTests(unittest.TestCase):
    def setUp(self):
        self.source = Path(__file__).resolve().parents[1]
        self.bash = os.environ.get("BASH_EXECUTABLE") or shutil.which("bash")
        self.git = shutil.which("git")
        if not self.bash or not self.git:
            self.skipTest("Bash and Git required")
        self.temp = tempfile.TemporaryDirectory(prefix="kufar-git-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.upstream = self.root / "upstream"
        self.project = self.root / "existing bot"
        self.upstream.mkdir()
        self.project.mkdir()
        self.env = dict(os.environ, GIT_TERMINAL_PROMPT="0")
        self.git_run(self.upstream, "init", "-b", "main")
        self.git_run(self.upstream, "config", "user.name", "Offline Test")
        self.git_run(self.upstream, "config", "user.email", "offline@example.invalid")
        self.git_run(self.upstream, "config", "core.autocrlf", "false")
        (self.upstream / ".gitignore").write_text(".env\ndata/\nbackups/\ncached-data.json\nupdate-ran\n", encoding="utf-8")
        (self.upstream / "src").mkdir()
        (self.upstream / "src" / "main.cpp").write_text("new code\n", encoding="utf-8")
        (self.upstream / "compose.yaml").write_text("services: {}\n", encoding="utf-8")
        (self.upstream / "kufar-configuration.json").write_text('{"queries":[]}', encoding="utf-8")
        (self.upstream / "update.sh").write_text("#!/usr/bin/env bash\nprintf 'updated\\n' >> update-ran\n", encoding="utf-8")
        self.commit("initial release")
        (self.project / ".env").write_text("KEEP_SECRET=yes\n", encoding="utf-8")
        (self.project / "kufar-configuration.json").write_text('{"queries":["keep search"]}', encoding="utf-8")
        (self.project / "compose.yaml").write_text("old compose\n", encoding="utf-8")
        (self.project / "src").mkdir()
        (self.project / "src" / "main.cpp").write_text("old code\n", encoding="utf-8")
        (self.project / "data").mkdir()
        (self.project / "data" / "cached-data.json").write_text('{"history":"keep"}', encoding="utf-8")
        (self.project / "cached-data.json").write_text("[123]", encoding="utf-8")
        self.env["KUFAR_REPOSITORY_URL"] = self.upstream.as_posix()

    def git_run(self, cwd, *args):
        return subprocess.run([self.git, *args], cwd=cwd, env=self.env, check=True,
                              capture_output=True, text=True, timeout=30).stdout

    def commit(self, message):
        self.git_run(self.upstream, "add", ".")
        self.git_run(self.upstream, "commit", "-m", message)

    def install(self):
        return subprocess.run([self.bash, self.source.joinpath("setup-git.sh").as_posix()],
                              cwd=self.project, env=self.env, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=60)

    def test_adopt_then_pull_and_develop_preserving_runtime_state(self):
        preserved = [".env", "kufar-configuration.json", "cached-data.json", "data/cached-data.json"]
        before = {name: (self.project / name).read_bytes() for name in preserved}
        old_source = (self.project / "src" / "main.cpp").read_bytes()
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.git_run(self.project, "branch", "--show-current").strip(), "main")
        self.assertEqual(self.git_run(self.project, "rev-parse", "HEAD"), self.git_run(self.upstream, "rev-parse", "HEAD"))
        self.assertEqual((self.project / "src" / "main.cpp").read_text(), "new code\n")
        self.assertEqual(self.git_run(self.project, "status", "--porcelain"), "")
        for name, body in before.items():
            self.assertEqual((self.project / name).read_bytes(), body, name)
        backups = list((self.project / "backups").glob("before-git-*.tar.gz"))
        self.assertEqual(len(backups), 1)
        with tarfile.open(backups[0]) as archive:
            self.assertEqual(archive.extractfile("src/main.cpp").read(), old_source)
            self.assertEqual(archive.extractfile(".env").read(), before[".env"])

        (self.upstream / "src" / "main.cpp").write_text("next release\n", encoding="utf-8")
        self.commit("next release")
        again = self.install()
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertEqual((self.project / "src" / "main.cpp").read_text(), "next release\n")
        self.assertEqual(len(list((self.project / "backups").glob("before-git-*.tar.gz"))), 1)
        self.assertEqual((self.project / "update-ran").read_text(), "updated\nupdated\n")
        for name, body in before.items():
            self.assertEqual((self.project / name).read_bytes(), body, name)
        (self.project / "src" / "main.cpp").write_text("my development\n", encoding="utf-8")
        self.assertIn("src/main.cpp", self.git_run(self.project, "status", "--porcelain"))

    def test_failed_clone_leaves_existing_installation_untouched(self):
        self.env["KUFAR_REPOSITORY_URL"] = self.root.joinpath("missing-repository").as_posix()
        result = self.install()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.project / ".git").exists())
        self.assertFalse((self.project / "backups").exists())
        self.assertEqual((self.project / "src" / "main.cpp").read_text(), "old code\n")


if __name__ == "__main__":
    unittest.main()
