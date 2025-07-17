<!--
  Thank you for contributing to the MariaDB Server repository!
  You can help us review your changes faster by completing this template. :orange_heart:
  If you have any questions related to MariaDB or only to meet other community members, please join us on https://mariadb.zulipchat.com/.
-->

<!--
  If you've already identified a https://jira.mariadb.org/ issue that tracks this bug/feature, please add its number below.
-->
- [ ] *The Jira issue number for this PR is: [MDEV-_____](https://jira.mariadb.org/browse/MDEV-_____)*

## Description
TODO: fill description here
<!--
  An excellent description should answer some questions like:
  1. What problem is the patch trying to solve?
  2. If some output changed that is not visible in a test case, what was it looking like before the change, and how does it look with this patch applied?
  3. Do you think this patch might introduce side effects in other parts of the server?
-->

## Release Notes
TODO: What should the release notes say about this change?
<!--
  Include any changed system variables, status variables or behaviour.
  List any https://mariadb.com/kb/ pages that need changing.
-->

## How to test this PR?
TODO: modify the automated test suite to verify that the PR causes MariaDB to behave as intended.
<!--
  Consult the documentation on ["Writing good test cases"](https://mariadb.org/get-involved/getting-started-for-developers/writing-good-test-cases-mariadb-server).
  In many cases, this will be as simple as modifying one `.test` and one `.result` file in the `mysql-test/` subdirectory.
  Without automated tests, future regressions in the expected behavior can't be automatically detected and verified.
-->
If the changes are not amenable to automated testing, please explain why and carefully describe how to test manually.

## Basing the PR on the correct MariaDB version
<!--
  Tick (`[x]`) one of the following boxes to help us understand if the base branch for the PR is correct.
-->
- [ ] *This is a new feature or a refactoring, and the PR is based on the `main` branch.*
- [ ] *This is a bug fix, and the PR is based on the earliest maintained branch in which the bug can be reproduced.*

## PR Quality Check
<!--
  All code merged into the MariaDB codebase must meet a quality standard and coding style.
  Maintainers are happy to point out inconsistencies, but to speed up the review and merge process, we ask you to verify the Coding Standards.
-->
- [ ] I checked the [CODING_STANDARDS.md](https://github.com/MariaDB/server/blob/-/CODING_STANDARDS.md) file, and my PR conforms to this where appropriate.
- [ ] For any trivial modifications to the PR, I am ok with the reviewer making the changes themselves.