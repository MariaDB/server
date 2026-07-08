# Community Contributions Process #

MariaDB Server is an Open Source project that thrives thanks to its community.
MariaDB Foundation handles all community contributions in a timely manner.

This document provides a description how community contributions are processed.
Please make sure to familiarize yourself and follow the process
when submitting contributions.

All community contribution pull requests will eventually get the
"External Contribution" label at some point.

## The two sides of a contribution

A contribution should have **BOTH** a design document
 in [Jira](https://jira.mariadb.org) and a pull request
 in [GitHub](https://github.com/MariaDB/server).

In the normal case the process usually starts with filing
 a [Jira](https://jira.mariadb.org) for the bug or the feature proposed.
Then a pull request gets created to implement the feature or fix the bug.
It could be the other way around too.
But it's important to always have both prior to review.

There are constant efforts (including background processes) to make sure
 the states of the Jira and the corresponding pull request(s) are kept in sync.

### What should go into the jira ###

The [Jira](https://jira.mariadb.org) is used to describe the design of the
feature: why is it a good idea, what should work, what might not work etc.
Or to describe the bug: what is wrong, how to recreate it,
 what's the expected outcome etc.
See [here](https://mariadb.org/contribute/#contribute-to-testing)
 for more details.

> [!TIP]
> For faster processing always fill all of the applicable Jira fields:
>  versions affected, component, etc.

> [!TIP]
> A lot of time an energy can be saved, especially on more complex tasks,
> by communicating early with possible reviewers and writing down a design
> specification into the Jira prior to implementation

> [!CAUTION]
> If the jira already exists and is assigned to somebody please reach out
> to them **before** starting to working on it to avoid effort duplication. 

### What should go into the pull request ###

The pull request is used to describe the implementation.
E.g. how the bug is fixed. Or how the feature is implemented in detail.

The pull request should contain a **single commit** per logical change!
And that commit should have a commit message that's compliant with
 the [MariaDB coding standards](https://github.com/MariaDB/server/blob/main/CODING_STANDARDS.md#git-commit-messages).

> [!IMPORTANT]
> Put the Jira reference, e.g. "MDEV-12345 ", as a prefix
> to your pull request's title

> [!CAUTION]
> If there's no Jira or no reference to it in your pull request
> there might be delays in processing it

## States of a Community Contribution ##

```mermaid
stateDiagram-v2
    [*] --> Draft
    Draft --> Closed : 3 Months of inactivity
    Draft --> Open
    Open --> Closed : Rejected
    state "Preliminary Review" as PR
    state "Final Review" as FR
    Open --> PR
    PR --> Draft : 21 Days of inactivity
    PR --> FR
    PR --> Closed : Rejected
    FR --> PR
    FR --> Draft : 21 Days of inactivity
    FR --> Approved
    FR --> Closed : Rejected
    Approved --> Merged
    Merged --> [*]
    Closed --> [*]
```

### Draft ###

* Pull request
  * State: Draft
  * Assignee: Not relevant
  * Label: optionally "need feedback" and/or "External Contribution"
* Jira
  * State: absent or in "Stalled"/"Open"/"Confirmed"
  * Assignee: Not relevant.

Contributions in this state are are excluded from any queues.
Except for the 3 months close period when they need feedback.
Put your contribution in Draft mode if it's not yet ready for
preliminary review.
Or if you want to sketch some sort of a pull request draft
to *manually* show to people.

At any time the pull request can be transitioned to "Open" state
by clicking the "Ready for review" button in
the GitHub pull request viewer UI.

> [!NOTE]
> There is a background process to move contributions in
> that state to "Closed" state if they have the "need feedback"
> and "External Contribution" pull request labels and there's
> been no activity for 3 months.

### Open ##

* Pull request
  * State: Open
  * Assignee: Not relevant
  * Label: not relevant
* Jira
  * State: "Stalled", "Confirmed" or "Open"
  * Assignee: Not relevant

Contributions in this state should be ready for review. This means:
* There should be a single commit per logical change in the pull request
  and this commit must
  * have a commit message that corresponds to the MariaDB Server coding
    standards, including the Jira reference.
  * contain a concise description of what the change is:
    this goes to the git commit log and is used for reference.
  * should contain all the relevant attribution headers
    (co-authors, reviewers etc.)
* The pull request should contain at least a copy of
  the commit message. Ideally more can be added to it.
* The pull request should target the
  right branch.
  Rule of thumb: the lowest affected but not older than three years
  GA branch for bugs, main branch for features and refactorings.
* All the relevant buildbot tests should be passing. Or, if failing,
  the failures should be studied, compared to the test results on the
  target branch (https://buildbot.mariadb.org/#/grid?branch=BRANCHNAME)
  and justified as unrelated.
* The licensing of the pull request should be clear. As a legal requirement,
  you must accept the CLA (contributor license agreement, also called MCA,
  MariaDB Contributor Agreement) or, alternatively, license your contribution
  under the 3-clause BSD license. See the
  [MariaDB Contributor Agreement FAQ](https://mariadb.com/docs/general-resources/community/community/legal-documents/mariadb-contributor-agreement-faq)
  for the difference between these two options and when to use what.

Questions and Answers
* When to move a contribution to Open state?
  * When you're ready to submit it to a preliminary review
* Who moves a contribution to Open state?
  * Usually the submitter
* What states can the contribution be moved to?
  * Preliminary review, when the preliminary review starts
  * Final review, if the final review starts before the preliminary
  * Closed, if rejected or has the "need feedback" pull request label and
    has been inactive for 3 months
* Who creates the Jira issue?
  * If there isn't one already, the submitter can and should create it


> [!NOTE]
> There is an asynchronous process that assigns "External Contribution"
> label to pull requests in this state, if absent.
> This marks the contribution entering the preliminary review queue.

### Preliminary Review ###

* Pull request
  * State: Open
  * Assignee: The preliminary reviewer
  * Label: External Contribution
* Jira
  * State: "Open", "In Progress", "Stalled" or "Needs feedback"
  * Assignee: The preliminary reviewer
  * Label: "Contribution"

Contributions enter this state usually from "Open" state.
The preliminary reviewer does the following:
  * Ensures that the contribution is ready for review: has jira,
    tests pass, standards are followed etc.
  * Provides a best effort technical review(s) of the content.
    Note that the preliminary reviewer is not a domain expert,
    so take that review with a grain of salt.
    This is done to save time the final reviewer's time.
  * Follows up so that their remarks are addressed.
  * Moves the contribution to Draft if the contributor is inactive
    for 3 weeks.
  * Moves the contribution to "Final Review" when done.
  * Assigns a "contribution" label to the Jira and makes sure
    it has "Critical" priority.
  * Can assign a "rework" label to community contribution pull requests.
    This means that reviews are done through some other process.
    Or that an internal developer has taken over the contribution to
    re-do it and push it directly.

### Final review ###

* Pull request
  * State: Open
  * Assignee: The current final reviewer
  * Label: External Contribution
* Jira
  * State: "In progress", "In review", "In testing", "Stalled",
    "Needs feedback"
  * Assignee: The final reviewer (when actively reviewing) or the
    preliminary reviewer, when waiting for reply by the contributior
    for too long
  * Label: "Contribution"

Contributions enter this state when the preliminary review is done.
Or if a certain domain expert takes interest into an incoming contribution
and starts interacting with it prior to the preliminary review.
The final reviewer is either a domain expert or a test engineer.
There can be more than one final reviewers assigned: usually done for
more complex contributions or when a separate testing step is required.

Watch the jira state and the assignees in this state for
detailed information:
  * "In review" means that somebody is assigned to review and they are
     working on it
  * "Stalled" means that they're waiting on something. Also watch
     the assignee in that state. If the jira is re-assigned back
     from a final reviewer to a preliminary reviewer, this means
     that the final reviewer waited for reply by the submitter
     for some time and have (temporarily) moved on by
     moving the Jira to "In progress" and assigning it back to the
     preliminary reviewer. When a reply is received the preliminary
     reviewer will move the jira back to "In review" or "In testing" and
     re-assign to the final reviewer that has requested the feedback.
  * "In testing" means a test engineer is assigned and they are working on
     testing this.
  * "Needs feedback" means that the reviewer needs feedback to proceed
  * "In progress" means that the preliminary reviewer has picked this up
     and is working on it.

**Try to avoid getting into the re-assignment loop by being active!**

> [!NOTE]
> There's a requirement that certain contributions need to undergo
> a separate test step. This is usually done for the new features or
> for more complex and risky bug fixes. If that's the desired state,
> a test engineer will be assigned as a final reviewer. See 
> https://mariadb.com/docs/server/reference/product-development/mariadb-quality-development-rules
> for more details.

> [!TIP]
> The final reviewer is usually a core server developer. They often work
> in cycles, timed to match the release schedule, switching between
> bug fixing and new feature development. If a bug fix PR comes
> in the middle of the new feature development cycle or vice versa
> it might take up to a couple of months for a developer to do a review.

### Approved ###

* Pull request
  * State: Open
  * Assignee: The last final reviewer
  * Label: External Contribution
* Jira
  * State: "Approved"
  * Assignee: The last final reviewer
  * Label: "Contribution"

Contributions in that state are ready to be merged. Usually it's the last
final reviewer's job to trigger the merge of the pull request and update
the Jira accordingly. But sometimes the preliminary reviewer can help with
that too.

Prior to merging the pull request it should be rebased to the latest in
the target branch. Thus the contributor should be watching for any
possible merge conflicts or test failures during that rebase.
The person that does the merge might need some assistance by the contributor.

When the pull request is successfully merged the Jira should be moved to
"Closed" state and its target version should be specified.

### Merged ###

* Pull request
  * State: Merged
  * Assignee: not applicable
  * Label: External Contribution
* Jira
  * State: "Closed"
  * Assignee: not applicable
  * Label: "Contribution"

This is the "success" final state for a contribution.
Ideally all contributions will end up in this state.
No further actions are necessary.

### Closed ###

* Pull request
  * State: Closed
  * Assignee: not applicable
  * Label: eventually "need feedback"
* Jira
  * State: "Closed"
  * Assignee: not applicable

This is the "failure" final state for a contribution.
A contribution can land in this state for one of the following reasons:
* It was rejected by the reviewers
* It was retracted by the contributor
* It was closed due to contributor's inactivity or lack of replies to
  questions, as signified by the presence of the "need feedback" label
   into the pull request.
