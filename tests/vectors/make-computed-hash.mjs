// Writes the test vectors for the project lock file's `computedHash` (docs/formats.md, section 3): SHA-256 folder hashes
// computed the way `npx skills` computes them (src/local-lock.ts, computeSkillFolderHash), including its file order,
// which comes from JavaScript's localeCompare. The C core must produce the same hashes.
//
//   node tests/vectors/make-computed-hash.mjs > tests/vectors/computed-hash.json
//
// The algorithm is copied rather than run from the npx package, so no downloaded code runs: collect regular files below
// the folder (skipping folders named .git and node_modules), sort by relative path with localeCompare, then feed each
// path and then its content to one SHA-256.
import { createHash } from 'node:crypto';

const content = (path) => `content of ${path}\n`;

const cases = [
    { name: 'ascii-case', files: ['SKILL.md', 'b.md', 'A.md', 'a.md', 'B.md', 'README', 'readme'] },
    { name: 'punctuation', files: ['SKILL.md', '_x.md', '-y.md', '.z', 'x y.md', 'x_y.md', 'x-y.md', 'x.y.md', 'x~y.md', 'x+y.md', '#a', '@b'] },
    { name: 'digits', files: ['SKILL.md', '10.md', '9.md', '1.md', 'a1.md', 'a10.md', 'a2.md', '01.md'] },
    { name: 'nested', files: ['SKILL.md', 'sub/Z.md', 'sub/y.md', 'sub-dir/a.md', 'sub.md', 'Sub/a.md', 'subz/a.md', 'sub/deeper/x.md'] },
    { name: 'skipped', files: ['SKILL.md', 'scripts/run.sh'], skipped: ['.git/HEAD', 'node_modules/x/index.js', 'deep/node_modules/y.js', 'deep/.git/config'] },
    { name: 'unicode', files: ['SKILL.md', 'ä.md', 'z.md', 'é.md', 'e.md', 'E.md', 'Ω.md', '日本.md', 'ß.md', 'ss.md'] },
    { name: 'realistic', files: ['SKILL.md', 'references/api.md', 'references/README.md', 'scripts/install.sh', 'scripts/Install.ps1', 'assets/logo.svg', 'LICENSE.txt', 'examples/basic/index.html'] },
];

const out = {
    generatedBy: `node ${process.version}, ICU ${process.versions.icu}, locale ${Intl.DateTimeFormat().resolvedOptions().locale}`,
    algorithm: 'sha256 over (relative path, then content) for each regular file, sorted with localeCompare; .git and node_modules folders skipped',
    cases: cases.map(({ name, files, skipped = [] }) => {
        const order = [...files].sort((a, b) => a.localeCompare(b));
        const hash = createHash('sha256');
        for (const path of order) {
            hash.update(path);
            hash.update(content(path));
        }
        return { name, files: files.map((path) => ({ path, content: content(path) })), skipped, order, computedHash: hash.digest('hex') };
    }),
};
process.stdout.write(JSON.stringify(out, null, 2) + '\n');
