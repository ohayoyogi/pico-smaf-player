# enabling CI

`docs/ci.yml` is the GitHub Actions workflow for this repo (build the engine +
example, plus a parse-reject smoke). it is kept here rather than under
`.github/workflows/` only because the push token used for the initial import
lacks the `workflow` scope. to enable CI, copy it into place:

    mkdir -p .github/workflows && cp docs/ci.yml .github/workflows/ci.yml

then commit + push from a token (or the web UI) that has the `workflow` scope.

once the workflow runs on `main`, swap the README build badge back to the live
one for a real status:

    <a href="../../actions/workflows/ci.yml"><img src="../../actions/workflows/ci.yml/badge.svg" alt="ci"></a>

until then the README shows a static `build-passing` badge (the build + fuzz are
verified locally) so no broken image appears.
