# Releasing

A release is a merge to `main` that changes `version` in `package.json`. CI then tags `vX.Y.Z`, publishes `voxel-plugin-tools@X.Y.Z` to npm with provenance and creates the GitHub release. Everything CI cannot check (the build and the live editor) is checked by hand before the merge, and the PR body is the only record that it was. Work through the steps in order; each one says how to confirm it.

CI's checks are `npm run check` and `npm audit`. It has no Unreal Editor, so nothing in CI compiles the C++ module or calls a handler.

## 1. Preconditions

1. The work is on a feature branch with an open PR against `main`. Nothing reaches `main` except through a PR.
2. The tree is clean: `git status` shows nothing staged, modified or untracked that belongs in the release.
3. The branch contains current `main`: `git fetch origin && git merge-base --is-ancestor origin/main HEAD` exits 0. If not, merge `origin/main` in before running the gates, because the gates must run on what will be merged.
4. Every GitHub issue the release resolves is named in the PR body as `Closes #N`. An issue closes only when the code that resolves it ships.
5. No other release PR is open with the same target version.

## 2. Version bump

1. Choose the version. The package is 0.x, and consumers depend on it with a caret range (`^0.7.1`), which in 0.x follows patch releases only:
   - **Patch** (`0.7.1` to `0.7.2`) is the default: fixes, docs, skills, descriptions, new optional parameters, new actions.
   - **Minor** (`0.7.x` to `0.8.0`) when an existing call stops working: an action renamed or removed, a parameter renamed, removed or made required, a contract rule tightened so a previously accepted call is refused, or `minServerVersion`, `minBridgeApi` or `supportedEngineVersions` raised. History: `0.6.0` (native-only category) and `0.7.0` (typed contracts). A minor bump needs the owner's explicit sign-off, as in ue-mcp core.
   - This is a judgement call; no tool classifies a change.
   - Never a prerelease suffix (`-beta.N`). CI has no prerelease routing: it publishes with no `--tag`, so a prerelease would not go to a separate channel.
2. Bump `package.json` and `package-lock.json` together:
   ```bash
   npm version X.Y.Z --no-git-tag-version
   ```
   Never create a tag; CI creates it.
3. Set `"VersionName": "X.Y.Z"` in `ue/Plugins/VoxelPluginTools/VoxelPluginTools.uplugin`. `.gitattributes` marks `*.uplugin` binary, so `git diff` hides this change; confirm it by reading the file. `npm run check` does not compare the two versions.
4. Commit the three files on their own, subject `X.Y.Z: <summary>`, matching the PR title (see step 5).
5. Confirm all three agree:
   ```bash
   node -p "require('./package.json').version + ' ' + require('./package-lock.json').version"
   grep VersionName ue/Plugins/VoxelPluginTools/VoxelPluginTools.uplugin
   ```

## 3. Static gate

```bash
npm install
npm run check
```

Must end `errors: 0` with every skill reported ok. It runs `scripts/check.mjs` (C0 to C7: the manifest against the host schema, C++ registrations against the manifest, routing names, descriptions, flows, read-only handlers, and every action name and call in the skills, knowledge file and README) and then `ue-mcp plugin check-skills .`. CI runs the same command on the PR and again before publishing (`prepublishOnly`).

## 4. Live gate

Run against `tests/voxel_plugin_tools/` only. Its uncommitted parts (`Plugins/Voxel`, `Plugins/UE_MCP_Bridge`, the `Plugins/VoxelPluginTools` junction to the working copy) are set up as described in CONTRIBUTING.md, Test project. If the `ue-mcp` devDependency changed in this release, refresh the test project's bridge first (`npx ue-mcp-deploy .` from `tests/voxel_plugin_tools`).

1. **One editor on the machine.** Confirm no Unreal Editor is running, for this project or any other: `tasklist /FI "IMAGENAME eq UnrealEditor.exe"` lists none. Two editors starve the machine and a build fails while Live Coding holds the module. If another session owns the running editor, wait; never close someone else's editor.
2. **Build** the test project with the editor closed:
   ```bash
   "<UE_5.8>/Engine/Build/BatchFiles/Build.bat" voxel_plugin_toolsEditor Win64 Development -Project=<abs path>/tests/voxel_plugin_tools/voxel_plugin_tools.uproject -WaitMutex
   ```
   Must end in success with no errors. The junction means this compiles the working copy.
3. **Start the editor** on the test project, from a ue-mcp session scoped to it, with `editor(action="start_editor")`; it returns only once the editor is ready. Then open an empty level (the startup map is `smoke_test`). `scripts/live-test.mjs` writes into a fresh `/Game/VoxelSmoke/Run<timestamp>` folder, which is gitignored; never save the level.
4. **Run the live test:**
   ```bash
   node scripts/live-test.mjs
   ```
   It calls every manifest handler straight through the bridge, checks every handler registered a contract with `save` declared exactly when its effect is `mutate`, and checks each contract rule is refused. It must print `N/N passed` and exit 0. One FAIL fails the release; there is no skip list.
5. **Check the recorded contracts:**
   ```bash
   npx ue-mcp plugin record-specs --project <abs path>/tests/voxel_plugin_tools/voxel_plugin_tools.uproject --check
   ```
   Must report `handler-specs.json matches the running module`. Run it on every release, not only when contracts changed: the editor is already up, and a hand edit to `handler-specs.json` is caught only here. If it fails because the contracts changed on purpose, run the same command without `--check`, commit `handler-specs.json`, rerun `npm run check`, and run `--check` again.
6. **Close the editor** with `editor(action="stop_editor")`, and confirm with `tasklist` that it is gone.

If any source file changes after step 2 (a fix found by the gate), the live gate restarts from step 1.

## 5. Pull request

Title: `X.Y.Z: <summary>`. CI creates the GitHub release with `--generate-notes`, which lists merged PR titles, so the title is the release note.

The body states:

- **Changes**, grouped (actions and contracts, docs and skills, tooling), each in one line. Name every contract or description change, because `handler-specs.json` ships them.
- **Closes #N** for each resolved issue.
- **Gates**, each with its command and result as printed:
  - `npm run check`: `errors: 0`.
  - Build: succeeded.
  - `node scripts/live-test.mjs`: `N/N passed`.
  - `record-specs --check`: matches (N handlers).
  - The `ue-mcp` version the gates ran against (the devDependency) and the Voxel `dev` commit of `tests/voxel_plugin_tools/Plugins/Voxel` (`git -C tests/voxel_plugin_tools/Plugins/Voxel rev-parse --short HEAD`).

A gate that was not run is stated as not run, with the reason; such a PR is not ready to merge. The PR's CI `build` job must be green.

## 6. Merge to main

A human approves and merges. Agents never push to or merge into `main`: the PreToolUse hook in `.claude/settings.json` blocks a `git push` that names `main`. It does not intercept `gh pr merge`, and `main` has no branch protection, so the merge rule is policy that the hook only partly enforces.

Merge style follows ue-mcp core: a PR that is one logical change is squash-merged; a PR of several independently meaningful commits is merged with a merge commit so each keeps its SHA.

## 7. Confirm what CI published

The push to `main` runs `.github/workflows/ci.yml`: the `build` job (`npm run check`, `npm audit --omit=dev --audit-level=high`), then the `publish` job. `publish` compares `package.json`'s version with `npm view voxel-plugin-tools version`; when they differ it tags `vX.Y.Z` and pushes the tag, runs `npm publish --access public --provenance` through npm trusted publishing (OIDC, no token), and creates the GitHub release with generated notes. When they are equal it does nothing, so a merge without a bump publishes nothing.

Confirm each, all from any machine:

```bash
gh run list --repo db-lyon/voxel-plugin-tools --branch main --limit 1         # completed, success
git ls-remote --tags origin vX.Y.Z                                            # the tag exists
npm view voxel-plugin-tools@X.Y.Z version dist.attestations.provenance         # the version, with provenance
npm view voxel-plugin-tools dist-tags.latest                                  # X.Y.Z
gh release view vX.Y.Z --repo db-lyon/voxel-plugin-tools                      # release exists, not a prerelease
```

If `publish` failed:

- **Before or at `npm publish`** (npm does not have the version): rerun the failed job, `gh run rerun <run-id> --failed`. The tag step skips an existing tag and the release step skips an existing release.
- **After `npm publish`** (npm has the version, the GitHub release is missing): a rerun does nothing, because the version check now finds the version published and skips every later step. Create the release by hand with the command CI uses: `gh release create vX.Y.Z --repo db-lyon/voxel-plugin-tools --title vX.Y.Z --generate-notes`.

## 8. Install in a consumer and verify

A consumer project has the package in its `node_modules`, an entry in `ue-mcp.yml`, the C++ module copied into `Plugins/VoxelPluginTools`, and the skills in `.claude/skills/voxel-plugin-tools-*`. Run from the consumer project's root, with its editor closed:

1. Install the release:
   ```bash
   npx ue-mcp plugin install voxel-plugin-tools --version X.Y.Z
   ```
   Use `install`, not `plugin update`. `install` runs `npm install --save`, rewrites the version pin in `ue-mcp.yml`, redeploys the C++ module into `Plugins/VoxelPluginTools` (pruning files the release stopped shipping, recorded in `.ue-mcp/native-modules.json`) and reinstalls the skills (pruning skills it stopped shipping, recorded in `.ue-mcp/skills.json`). `plugin update` only runs `npm update` and refreshes skills: it leaves the old C++ in `Plugins/`, follows the caret range (so never crosses a minor), and if `ue-mcp.yml` pins a version the server then skips the plugin with a version-mismatch error.
2. Rebuild: `npx ue-mcp build`. Must succeed.
3. Confirm the files:
   ```bash
   node -p "require('./node_modules/voxel-plugin-tools/package.json').version"   # X.Y.Z
   grep VersionName Plugins/VoxelPluginTools/VoxelPluginTools.uplugin            # X.Y.Z
   npx ue-mcp plugin list                                                         # voxel-plugin-tools@X.Y.Z (pinned X.Y.Z) - ok
   ls .claude/skills | grep voxel-plugin-tools-                                   # exactly the skills in this repo's skills/
   ```
4. Restart the MCP client so the server reloads the plugin, start the editor, then confirm the live surface:
   - `plugins(action="list")` reports `voxel-plugin-tools` at `X.Y.Z`, status `active`, nothing under `degraded`. This version is read from the server's copy of the package, not from the editor.
   - The bridge reports no version for the native module. To prove the editor runs the released C++, compare its live contracts with the ones the package ships:
     ```bash
     npx ue-mcp plugin record-specs node_modules/voxel-plugin-tools --project <abs path to the consumer .uproject> --check
     ```
     Must report a match. It only reads the editor's capabilities.
   - `voxel(action="voxel_shader_hooks_status")` succeeds.

## 9. Rollback

Never unpublish: npm never accepts a version number again once it is used, and CI's publish check keys on the registry. Fix forward.

1. Deprecate the bad version, from a machine logged in to npm as an owner (`npm owner ls voxel-plugin-tools`). Trusted publishing covers only CI's `npm publish`, so this is a manual, authenticated step:
   ```bash
   npm deprecate voxel-plugin-tools@X.Y.Z "Broken: <one line>. Use X.Y.(Z+1)."
   ```
2. Revert the offending change on a branch (`git revert <sha>`; for a merge commit `git revert -m 1 <sha>`), bump to the next patch, and run this whole procedure again. Do not move the `latest` dist-tag back by hand: `main` would then carry a version that differs from `latest`, and the next push to `main` would try to republish an existing version and fail.
3. Keep the tag and the GitHub release; they are cited. Edit the release body to say it is deprecated and name the fix version: `gh release edit vX.Y.Z --notes-file <file>`.
4. Consumers that installed the bad version run step 8 with the fixed version.
