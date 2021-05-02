const {promisify} = require('util');
const {resolve, relative, basename} = require('path');
const fs = require('fs');
const {EOL} = require('os');
const {gzipSync} = require('zlib');
const {contentType} = require('mime-types');
const commandLineArgs = require('command-line-args');
const filesize = require('filesize');

const readdir = promisify(fs.readdir);
const stat = promisify(fs.stat);

const optionDefinitions = [
  {name: 'src', alias: 'i', type: String},
  {name: 'dst', alias: 'o', type: String},
  {name: 'namespace', alias: 'n', type: String},
  {name: 'mapname', alias: 'm', type: String},
];

const options = commandLineArgs(optionDefinitions);
if (!options.src) {
  console.error('Missing src option');
  process.exit();
}

if (!options.dst) {
  console.error('Missing dst option');
  process.exit();
}

if (!options.namespace) {
  console.error('Missing namespace option');
  process.exit();
}

if (!options.mapname) {
  console.error('Missing mapname option');
  process.exit();
}

const src = resolve(process.cwd(), options.src);

console.log(`Bundling files of folder ${src}`);

// Recursively get all files in a directory
async function getFiles(dir) {
  const subdirs = await readdir(dir);
  const files = await Promise.all(subdirs.map(async (subdir) => {
    const res = resolve(dir, subdir);
    return (await stat(res)).isDirectory() ? getFiles(res) : res;
  }));
  return files.reduce((a, f) => a.concat(f), []);
}


// Get all files in the source folder
getFiles(src).then((files) => {
  // Add the header of the file
  let data = '#include "static_files/files.hpp"' + EOL;
  data += '#include <gzip/decompress.hpp>' + EOL + EOL;
  data += `namespace ${options.namespace} {` + EOL;

  // Create the declaration of the map containing the uncompressed files
  let map = `const file_map_t &${options.mapname}() {` + EOL;
  map += '\tstatic const file_map_t files{' + EOL;

  // Iterate the files in the folder
  files.forEach((file, index) => {
    // Get the relative path
    const rel = relative(src, file).replace('\\', '/');

    // Read the contents of the file into a buffer
    let buf = fs.readFileSync(file);
    const origSize = buf.length;

    // Compress the buffer
    buf = gzipSync(buf);

    // Print the contents of the buffer
    const comment = `// File: ${rel} (${filesize(origSize)} / ${
                        filesize(buf.length)} compressed)` +
        EOL;
    data += comment;
    data += `\tstatic const uint8_t fileData${index}[] = {` + EOL;
    for (let i = 0; i < buf.length;) {
      data += '\t\t';
      // Add the bytes in rows of 20
      for (let j = 0; j < 20 && i < buf.length; ++j, ++i) {
        data += `0x${buf[i].toString(16).padStart(2, '0')}, `;
      }
      data += EOL;
    }
    data += '};' + EOL + EOL;

    // Add an entry to the map that decompresses the file into the map
    map += '\t\t' + comment;
    map += `\t\t{ "${rel}", {` + EOL;
    map += `\t\t\t\t{(const char*)fileData${index}, ${buf.length}},` + EOL;
    map += `\t\t\t\tgzip::decompress((const char*)fileData${index}, ${
               buf.length}),` +
        EOL;
    map += `\t\t\t\t"${contentType(basename(rel))}"` + EOL;
    map += `\t\t\t}` + EOL;
    map += '\t\t},' + EOL;
  });

  // Terminate the map declaration and add it to the file
  map += '\t};' + EOL;
  map += '\treturn files;' + EOL;
  map += '};' + EOL;
  data += map;

  // Add termination of the namespace to the file
  data += `} // namespace ${options.namespace}` + EOL;

  // Write the file buffer to disk
  fs.writeFileSync(resolve(process.cwd(), options.dst), data);
});
