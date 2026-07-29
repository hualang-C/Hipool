#!/usr/bin/env node
/**
 * batch_learn.js -- hipool adaptive lexicon batch import
 *
 * Strategy: write lexicon entries to hipool using the memory set command.
 *
 * Usage:
 *   node batch_learn.js --dry-run    Preview
 *   node batch_learn.js              Write to hipool
 */

const { execSync } = require('child_process');
const fs = require('fs');
const path = require('path');

// ========== Config ==========

const HIPOOL_DIR = __dirname;
const MEMORY = path.join(HIPOOL_DIR, '../build/memory');

// Users can add their own terms here:
// { w: 'word', e: entity_id }  -- entity 18=brand/company, 19=tech/framework, 0=general concept
const TERMS = [];

// ========== Utils ==========

function sh(cmd) {
  try { return execSync(cmd, { encoding: 'utf-8', maxBuffer: 5*1024*1024, timeout: 10000 }).trim(); }
  catch (e) { return (e.stdout || '').trim() || ''; }
}

// ========== Main ==========

(function() {
  var dryRun = process.argv.indexOf('--dry-run') >= 0;

  console.log('========================================');
  console.log('  hipool Batch Lexicon Import');
  console.log('========================================');

  if (TERMS.length === 0) {
    console.log('  No terms defined. Edit the TERMS array at the top of this script.');
    console.log('========================================');
    return;
  }

  console.log('  Total terms: ' + TERMS.length);
  console.log('');

  if (dryRun) {
    console.log('  (Preview mode, not written -- run without --dry-run to execute)');
    console.log('========================================');
    return;
  }

  // Write: use memory set to store terms as data
  // Format: lexicon:<word> -> {"entity": N, "tag": "M", "source": "batch_learn"}
  console.log('  Writing to hipool...');
  var ok = 0;
  for (var i = 0; i < TERMS.length; i++) {
    var t = TERMS[i];
    var val = JSON.stringify({ entity: t.e, tag: 'M' });
    sh('"' + MEMORY + '" set "lexicon:' + t.w + '" ' + val + ' --tags "learned,entity_' + t.e + '"');
    ok++;
  }

  // flush
  sh('"' + MEMORY + '" flush');

  console.log('  Write complete: ' + ok + '/' + TERMS.length);
  console.log('');
  console.log('========================================');
})();
