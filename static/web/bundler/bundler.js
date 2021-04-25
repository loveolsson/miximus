const {promisify} = require('util');
const {resolve, relative} = require('path');
const fs = require('fs');
const {EOL} = require('os');
const {gzipSync} = require('zlib');
const readdir = promisify(fs.readdir);
const stat = promisify(fs.stat);

// Recursively get all files in a directory
async function getFiles(dir) {
  const subdirs = await readdir(dir);
  const files = await Promise.all(subdirs.map(async (subdir) => {
    const res = resolve(dir, subdir);
    return (await stat(res)).isDirectory() ? getFiles(res) : res;
  }));
  return files.reduce((a, f) => a.concat(f), []);
}

// Get all files in the 'dist' folder
getFiles('./dist').then((files) => {
  // Add the header of the file
  let data = '#include "static_files/files.hpp"' + EOL;
  data += '#include <gzip/decompress.hpp>' + EOL + EOL;
  data += 'namespace miximus::static_files {' + EOL;

  // Create the declaration of the map containing the uncompressed files
  let map =
      'const std::unordered_map<std::string, std::string> webFiles = {' + EOL;

  // Iterate the files in the folder
  files.forEach((file, index) => {
    // Get the relative path
    const rel = relative('./dist', file).replace('\\', '/');

    // Read the contents of the file into a buffer
    let buf = fs.readFileSync(file);

    // Compress the buffer
    buf = gzipSync(buf);

    // Print the contents of the buffer
    data += `// File: ${rel}` + EOL;
    data += `static const uint8_t fileData${index}[] = {` + EOL;
    for (let i = 0; i < buf.length;) {
      data += '\t';
      // Add the bytes in rows of 20
      for (let j = 0; j < 20 && i < buf.length; ++j, ++i) {
        data += `0x${buf[i].toString(16).padStart(2, '0')}, `;
      }
      data += '' + EOL;
    }
    data += '};' + EOL + EOL;

    // Add an entry to the map that decompresses the file into the map
    map += `\t{ "${rel}", gzip::decompress((const char*)fileData${index}, ${
               buf.length}) },` +
        EOL;
  });

  // Terminate the map declaration and add it to the file
  map += '};' + EOL + EOL;
  data += map;

  // Add termination of the namespace to the file
  data += '} // namespace miximus::static_files' + EOL;

  // Create the folder if it doesn't exist
  const dir = './dist-bundle';
  if (!fs.existsSync(dir)) {
    fs.mkdirSync(dir);
  }

  // Write the file buffer to disk
  fs.writeFileSync(resolve(dir, 'dist.cpp'), data);
});
