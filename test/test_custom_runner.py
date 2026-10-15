"""PlatformIO runner for the host test suites: `pio test -e native -e native_tls`.

Each suite (test/<suite>/*.cpp) is a plain C++ program with its own main(). It
prints "FAIL <file>:<line>  <expr>" for every failed check and "OK: ..." when all
pass, and exits non-zero on failure (PlatformIO already reports a non-zero exit
as an error, so a crash or a missed FAIL line still fails the run).
"""

from platformio.public import TestCase, TestRunnerBase, TestStatus

FAIL_PREFIX = "FAIL "
OK_PREFIX = "OK:"


class CustomTestRunner(TestRunnerBase):
    def on_testing_line_output(self, line: str) -> None:
        super().on_testing_line_output(line)  # echo the program's output
        text = line.strip()
        if text.startswith(FAIL_PREFIX):
            self.test_suite.add_case(
                TestCase(name=text[len(FAIL_PREFIX):], status=TestStatus.FAILED)
            )
        elif text.startswith(OK_PREFIX):
            self.test_suite.add_case(
                TestCase(name=text[len(OK_PREFIX):], status=TestStatus.PASSED)
            )
