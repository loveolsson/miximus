const { promisify } = require('util');
const { resolve, relative } = require('path');
const fs = require('fs');
const readdir = promisify(fs.readdir);
const stat = promisify(fs.stat);
const {EOL} = require('os');

async function getFiles(dir) {
  const subdirs = await readdir(dir);
  const files = await Promise.all(subdirs.map(async (subdir) => {
    const res = resolve(dir, subdir);
    return (await stat(res)).isDirectory() ? getFiles(res) : res;
  }));
  return files.reduce((a, f) => a.concat(f), []);
}

getFiles('./dist').then(files => {
    let data = '#include "static_files/files.hpp"' + EOL;
    data += 'namespace miximus::static_files {' + EOL;;

    let map = 'const std::unordered_map<std::string_view, std::string_view> files = {' + EOL;;

    files.forEach((file, index) => {
        const rel = relative('./dist', file);
        const buf = fs.readFileSync(file);
        
        data += `// File: ${rel}` + EOL;
        data += `static const uint8_t fileData${index}[] = {` + EOL;
        for (let i = 0; i < buf.length; ) {
            data += '\t'
            for (let j = 0; j < 20 && i < buf.length; ++j, ++i) {
                data += `0x${buf[i].toString(16).padStart(2, '0')}, `;
            }
            data += '' + EOL;
        }
        data += '};' + EOL;

        map += `\t{ std::string_view("${rel.replace('\\', '/')}"), std::string_view((const char*)fileData${index}, (size_t)${buf.length}) },` + EOL;
    });

    map += '};' + EOL;;

    data += map;

    data += '} // namespace miximus::static_files' + EOL;;

    const dir = './dist-bundle';
    if (!fs.existsSync(dir)){
        fs.mkdirSync(dir);
    }
    fs.writeFileSync(resolve(dir,'dist.cpp'), data);
});

