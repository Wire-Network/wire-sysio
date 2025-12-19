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
const batch_size = parseInt(core.getInput('batch-size') || '10');

const repo_name = process.env.GITHUB_REPOSITORY.split('/')[1];
const dockerHost = process.env.DOCKER_HOST || '';
const dockerArgs = dockerHost ? ['--host', dockerHost] : [];

async function main() {
   try {
      const suffix = `${process.env.GITHUB_RUN_ID}-${process.env.GITHUB_JOB}-${Math.floor(Math.random() * 10000)}`;
      const baseContainer = `base-${suffix}`;
      const baseImage = `baseimage-${suffix}`;

      console.log(`=== Setup Info ===`);
      console.log(`Docker Host: ${dockerHost || 'default'}`);
      console.log(`Docker Args: ${dockerArgs.join(' ') || 'none'}`);
      console.log(`Base Container: ${baseContainer}`);
      console.log(`Base Image: ${baseImage}`);
      console.log(`Working Directory: ${process.cwd()}`);
      console.log(`Repository: ${repo_name}`);
      console.log(`Batch Size: ${batch_size}`);
      console.log(`==================\n`);

      child_process.spawnSync("docker", [...dockerArgs, "rm", "-f", baseContainer], {stdio:"ignore"});

      console.log("Creating base container...");
      if(child_process.spawnSync("docker", [...dockerArgs, "run", "--name", baseContainer, "-v", `${process.cwd()}/build.tar.gz:/build.tar.gz`, "--workdir", `/__w/${repo_name}/${repo_name}`, container, "sh", "-c", "tar xzf /build.tar.gz"], {stdio:"inherit"}).status)
         throw new Error("Failed to create base container");
      
      console.log("Committing base image...");
      if(child_process.spawnSync("docker", [...dockerArgs, "commit", baseContainer, baseImage], {stdio:"inherit"}).status)
         throw new Error("Failed to create base image");
      
      console.log("Removing temporary base container...");
      if(child_process.spawnSync("docker", [...dockerArgs, "rm", baseContainer], {stdio:"inherit"}).status)
         throw new Error("Failed to remove base container");

      console.log("\nDiscovering tests...");
      const test_query_result = child_process.spawnSync("docker", [...dockerArgs, "run", "--rm", baseImage, "bash", "-e", "-o", "pipefail", "-c", `cd build; ctest -L '${tests_label}' --show-only=json-v1`]);
      if(test_query_result.status) {
         console.error("STDOUT:", test_query_result.stdout.toString());
         console.error("STDERR:", test_query_result.stderr.toString());
         throw new Error("Failed to discover tests with label")
      }
      const tests = JSON.parse(test_query_result.stdout).tests;
      console.log(`Found ${tests.length} tests with label '${tests_label}'`);
      tests.forEach((t, i) => console.log(`  ${i+1}. ${t.name}`));
      console.log();

      // Run a quick sanity check on the first test
      if (tests.length > 0) {
         console.log(`\n=== Sanity check: Testing environment for ${tests[0].name} ===`);
         const sanityCheck = child_process.spawnSync("docker", [...dockerArgs, "run", "--rm", baseImage, "bash", "-c", 
            `cd build && \
             echo "PWD: $(pwd)" && \
             echo "Build dir exists: $(test -d . && echo yes || echo no)" && \
             echo "Testing dir exists: $(test -d Testing && echo yes || echo no)" && \
             echo "Can create Temporary: $(mkdir -p Testing/Temporary && echo yes || echo no)" && \
             echo "CTest available: $(which ctest)" && \
             ls -la Testing/ 2>/dev/null || echo "No Testing directory"`
         ], {stdio:"inherit"});
         console.log(`=== Sanity check complete ===\n`);
      }

      const totalBatches = Math.ceil(tests.length / batch_size);
      console.log(`Starting parallel test execution in ${totalBatches} batch(es) of up to ${batch_size} tests...\n`);

      const results = [];


      for (let batchNum = 0; batchNum < totalBatches; batchNum++) {
         const start = batchNum * batch_size;
         const end = Math.min(start + batch_size, tests.length);
         const batch = tests.slice(start, end);
         
         console.log(`\n${'='.repeat(60)}`);
         console.log(`BATCH ${batchNum + 1}/${totalBatches}: Running tests ${start + 1}-${end} of ${tests.length}`);
         console.log(`${'='.repeat(60)}\n`);

         const subprocesses = batch.map(t => new Promise(resolve => {
            const cname = `${t.name}-${suffix}`
            const args = [...dockerArgs, 'run', '--security-opt', 'seccomp=unconfined',
               '-e', 'GITHUB_ACTIONS=True', '--name', cname, '--init', baseImage,
               'bash', '-c', `cd build; ctest --output-on-failure -R '^${t.name}$' --timeout ${test_timeout}`]
            
            console.log(`[${t.name}] Starting container ${cname}`);
            
            const proc = child_process.spawn('docker', args, { stdio: 'pipe' });
            
            let stdout = '';
            let stderr = '';
            
            proc.stdout.on('data', (data) => {
               const output = data.toString();
               stdout += output;
          
               output.split('\n').forEach(line => {
                  if (line) console.log(`[${t.name}] ${line}`);
               });
            });
            
            proc.stderr.on('data', (data) => {
               const output = data.toString();
               stderr += output;
               output.split('\n').forEach(line => {
                  if (line) console.error(`[${t.name}] ERR: ${line}`);
               });
            });
            
            proc.on('close', code => {
               if (code !== 0) {
                  console.error(`\n=== Test ${t.name} FAILED with exit code ${code} ===`);
                  if (stderr) {
                     console.error('STDERR:');
                     console.error(stderr);
                  }
                  console.error('================================\n');
               } else {
                  console.log(`[${t.name}] ✓ PASSED`);
               }
               resolve({ name: t.name, code, stdout, stderr })
            });
            
            proc.on('error', (err) => {
               console.error(`[${t.name}] Process error:`, err);
               resolve({ name: t.name, code: -1, stdout, stderr, error: err.message });
            });
         }));

         const batchResults = await Promise.all(subprocesses);
         results.push(...batchResults);

         const batchPassed = batchResults.filter(r => r.code === 0).length;
         const batchFailed = batchResults.filter(r => r.code !== 0).length;
         console.log(`\n--- Batch ${batchNum + 1} Summary: ${batchPassed} passed, ${batchFailed} failed ---\n`);
      }

      console.log('\n==== Final test results ====')
      const passed = results.filter(r => r.code === 0).length;
      const failed = results.filter(r => r.code !== 0).length;
      console.log(`Total: ${results.length} tests, ${passed} passed, ${failed} failed\n`);
      
      results.forEach(r => {
         const status = r.code === 0 ? '✓ PASS' : '✗ FAIL';
         console.log(`${status} Test ${r.name} → exit ${r.code}`);
         if (r.error) {
            console.log(`      Error: ${r.error}`);
         }
      });
      console.log('================================\n')


      let hasFailures = false;
      console.log("Extracting logs from failed tests...");
      
      for(let i = 0; i < results.length; ++i) {
         if(results[i].code === 0)
            continue;

         hasFailures = true;
         const containerName = `${tests[i].name}-${suffix}`;
         
         console.log(`Extracting logs from ${containerName}...`);

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
            console.log(`  Including: ${header.name}`);
            stream.pipe(packer.entry(header, next));
         }).on('finish', () => {packer.finalize()});

         child_process.spawn("docker", [...dockerArgs, "export", containerName]).stdout.pipe(extractor);
         await stream.promises.pipeline(packer, zlib.createGzip(), fs.createWriteStream(`${log_tarball_prefix}-${tests[i].name}-logs.tar.gz`));
         console.log(`  Created: ${log_tarball_prefix}-${tests[i].name}-logs.tar.gz`);
      }

      if(hasFailures) {
         console.error("\n❌ Some tests failed - check logs above for details");
         core.setFailed("Some tests failed");
      } else {
         console.log("\n✓ All tests passed!");
      }

   } catch(e) {
      console.error("\n=== UNCAUGHT EXCEPTION ===");
      console.error("Message:", e.message);
      console.error("Stack:", e.stack);
      console.error("==========================\n");
      core.setFailed(`Uncaught exception ${e.message}`);
   }
}

main()