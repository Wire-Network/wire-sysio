# Github Actions Snippets

Code & shell snippets for Github Actions workflows.

## Debugging actions on your local machine

### Prerequisites
- [Docker](https://www.docker.com/)
- [act](https://github.com/nektos/act), one line install `curl -s https://raw.githubusercontent.com/nektos/act/master/install.sh | sudo bash`
- A Github Token in your local environment as `GITHUB_TOKEN`

### Setup

Act uses events and secrets from `.github/act` directory.

The event schema is the same as the one used by Github Actions, an example PR event is
located at [event-pull-request.json](../.github/act/event-pull-request.json).

### Usage

To debug actions locally, run the following command:
```bash
# Run `pull_request` workflow
act --verbose pull_request \
  -e .github/act/event-pull-request.json \
  -s GH_TOKEN_DEV=$GITHUB_TOKEN \
  -s GH_TOKEN=$GITHUB_TOKEN \
  -s GITHUB_TOKEN=$GITHUB_TOKEN
```
Replace `<job_name>` with the name of the job you want to debug.
