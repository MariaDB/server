<!--
Thank you for contributing to the MariaDB Server repository!

You can help us review your changes faster by filling in this template <3

If you have any questions related to MariaDB or you just want to hang out and meet other community members, please join us on https://mariadb.zulipchat.com/ .
-->

<!--
If you've already identified a https://jira.mariadb.org/ issue that seems to track this bug/feature, please add its number below.
-->
- [x] *The Jira issue number for this PR is: MDEV-______*

<!--
An amazing description should answer some questions like:
1. What problem is the patch trying to solve?
2. If some output changed that is not visible in a test case, what was it looking like before the change and how it's looking with this patch applied?
3. Do you think this patch might introduce side-effects in other parts of the server?
-->
## Description
TODO: fill description here

## Release Notes
TODO: What should the release notes say about this change?
Include any changed system variables, status variables or behaviour. Optionally list any https://mariadb.com/kb/ pages that need changing.

## How can this PR be tested?

TODO: modify the automated test suite to verify that the PR causes MariaDB to behave as intended.
Consult the documentation on ["Writing good test cases"](https://mariadb.org/get-involved/getting-started-for-developers/writing-good-test-cases-mariadb-server).
<!--
In many cases, this will be as simple as modifying one `.test` and one `.result` file in the `mysql-test/` subdirectory.
Without automated tests, future regressions in the expected behavior can't be automatically detected and verified.
-->

If the changes are not amenable to automated testing, please explain why not and carefully describe how to test manually.

<!--
Tick one of the following boxes [x] to help us understand if the base branch for the PR is correct.
-->
## Basing the PR against the correct MariaDB version
- [ ] *This is a new feature or a refactoring, and the PR is based against the `main` branch.*
- [ ] *This is a bug fix, and the PR is based against the earliest maintained branch in which the bug can be reproduced.*

<!--
  All code merged into the MariaDB codebase must meet a quality standard and coding style.
  Maintainers are happy to point out inconsistencies but in order to speed up the review and merge process we ask you to check the CODING standards.
-->
## PR quality check
- [ ] I checked the [CODING_STANDARDS.md](https://github.com/MariaDB/server/blob/-/CODING_STANDARDS.md) file and my PR conforms to this where appropriate.
- [ ] For any trivial modifications to the PR, I am ok with the reviewer making the changes themselves.
