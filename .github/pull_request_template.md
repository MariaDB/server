<!--
Thank you for contributing to the MariaDB Server repository!

You can help us review your changes faster by filling this template <3

If you have any questions related to MariaDB or you just want to
hang out and meet other community members, please join us on
https://mariadb.zulipchat.com/ .
-->

<!--
If you've already identified a https://jira.mariadb.org/ issue
that seems to track this bug/feature, please add its number below.
-->
- [x] *The Jira issue number for this PR is: MDEV-_____*

<!--
An amazing description should answer some questions like:
1. What problem is the patch trying to solve?
2. If some output changed, what was it looking like before
   the change and how it's looking with this patch applied
3. Do you think this patch might introduce side-effects in
   other parts of the server?
-->
## Description
TODO: fill description here

## How can this PR be tested?

TODO: modify the automated test suite to verify that the PR causes MariaDB to
behave as intended. Consult the documentation on
["Writing good test cases"](https://mariadb.org/get-involved/getting-started-for-developers/writing-good-test-cases-mariadb-server).
In many cases, this will be as simple as modifying one `.test` and one `.result`
file in the `mysql-test/` subdirectory. Without _automated_ tests, future regressions
in the expected behavior can't be automatically detected and verified.

If the changes are not amenable to automated testing, please explain why not and
carefully describe how to test manually.

<!--
Tick one of the following boxes [x] to help us understand
if the base branch for the PR is correct
-->
## Basing the PR against the correct MariaDB version
- [ ] *This is a new feature and the PR is based against the latest MariaDB development branch*
- [ ] *This is a bug fix and the PR is based against the earliest branch in which the bug can be reproduced*

<!--
You might consider answering some questions like:
1. Does this affect the on-disk format used by MariaDB?
2. Does this change any behavior experienced by a user
   who upgrades from a version prior to this patch?
3. Would a user be able to start MariaDB on a datadir
   created prior to your fix?
-->
## Backward compatibility
TODO: fill details here, if applicable, or remove the section
