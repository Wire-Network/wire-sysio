// Build command:
// npx esbuild .github/actions/parallel-ctest-containers/main.mjs \
//   --bundle --platform=node --target=node20 --format=cjs \
//   --outfile=.github/actions/parallel-ctest-containers/dist/index.js

import child_process from 'node:child_process';
import process from 'node:process';
import stream from 'node:stream';
import fs from 'node:fs';
import zlib from 'node:zlib';
import tar from 'tar-stream';
import core from '@actions/core'

const container = core.getInput('container', {required: true});
const error_log_paths = JSON.parse(core.getInput('error-log-paths', {required: true}));
const log_tarball_prefix = core.getInput('log-tarball-prefix', {required: true});
const tests_label = core.getInput('tests-label', {required: true});
const test_timeout = core.getInput('test-timeout', {required: true});

const repo_name = process.env.GITHUB_REPOSITORY.split('/')[1];
const dockerHost = process.env.DOCKER_HOST || '';
const dockerArgs = dockerHost ? ['--host', dockerHost] : [];

async function main() {
   try {
      const suffix = `${process.env.GITHUB_RUN_ID}-${process.env.GITHUB_JOB}-${Math.floor(Math.random() * 10000)}`;
      const baseContainer = `base-${suffix}`;
      const baseImage = `baseimage-${suffix}`;

      child_process.spawnSync("docker", [...dockerArgs, "rm", "-f", baseContainer], {stdio:"ignore"});

      if(child_process.spawnSync("docker", [...dockerArgs, "run", "--name", baseContainer, "-v", `${process.cwd()}/build.tar.zst:/build.tar.zst`, "--workdir", `/__w/${repo_name}/${repo_name}`, container, "sh", "-c", "zstdcat /build.tar.zst | tar x"], {stdio:"inherit"}).status)
         throw new Error("Failed to create base container");
      if(child_process.spawnSync("docker", [...dockerArgs, "commit", baseContainer, baseImage], {stdio:"inherit"}).status)
         throw new Error("Failed to create base image");
      if(child_process.spawnSync("docker", [...dockerArgs, "rm", baseContainer], {stdio:"inherit"}).status)
         throw new Error("Failed to remove base container");

      const test_query_result = child_process.spawnSync("docker", [...dockerArgs, "run", "--rm", baseImage, "bash", "-e", "-o", "pipefail", "-c", `cd build; ctest -L '${tests_label}' --show-only=json-v1`]);
      if(test_query_result.status)
         throw new Error("Failed to discover tests with label")
      const tests = JSON.parse(test_query_result.stdout).tests;

    const subprocesses = tests.map(t => new Promise(resolve => {
      const cname = `${t.name}-${suffix}`
      const args = [...dockerArgs, 'run', '--security-opt', 'seccomp=unconfined',
        '-e', 'GITHUB_ACTIONS=True', '--name', cname, '--init', baseImage,
        'bash', '-c', `cd build; ctest --output-on-failure -R '^${t.name}$' --timeout ${test_timeout}`]
      child_process.spawn('docker', args, { stdio: 'inherit' })
        .on('close', code => resolve({ name: t.name, code }))
    }))

      const results = await Promise.all(subprocesses);

      console.log('==== Parallel test results ====')
      results.forEach(r => console.log(`Test ${r.name} → exit ${r.code}`))
      console.log('================================')


      let hasFailures = false;
      for(let i = 0; i < results.length; ++i) {
         if(results[i] === 0)
            continue;

         hasFailures = true;
         const containerName = `${tests[i].name}-${suffix}`;

         let extractor = tar.extract();
         let packer = tar.pack();
         extractor.on('entry', (header, stream, next) => {
            if(!header.name.startsWith(`__w/${repo_name}/${repo_name}/build`)) {
               stream.on('end', () => next());
               stream.resume();
               return;
            }
            header.name = header.name.substring(`__w/${repo_name}/${repo_name}/`.length);
            if(header.name !== "build/" && error_log_paths.filter(p => header.name.startsWith(p)).length === 0) {
               stream.on('end', () => next());
               stream.resume();
               return;
            }
            stream.pipe(packer.entry(header, next));
         }).on('finish', () => {packer.finalize()});

         child_process.spawn("docker", [...dockerArgs, "export", containerName]).stdout.pipe(extractor);
         stream.promises.pipeline(packer, zlib.createGzip(), fs.createWriteStream(`${log_tarball_prefix}-${tests[i].name}-logs.tar.gz`));
      }

      if(hasFailures) 
         core.setFailed("Some tests failed");

   } catch(e) {
      core.setFailed(`Uncaught exception ${e.message}`);
   }
}

main()