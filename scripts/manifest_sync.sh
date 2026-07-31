#!/usr/bin/env bash
# 自动同步所有 managed template 文件的 manifest content_sha256
# 用法: scripts/manifest_sync.sh
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
node -e '
const fs=require("fs"),crypto=require("crypto"),path=require("path");
const root=process.cwd();
const mf=path.join(root,".ai-harness/manifest.json");
const m=JSON.parse(fs.readFileSync(mf));
const sha=f=>{const h=crypto.createHash("sha256").update(fs.readFileSync(f)).digest("hex");return"sha256:"+h;};
let fixed=0;
for(const e of m.managed_paths){
  if(e.ownership!=="template")continue;
  const f=path.join(root,e.path);
  let actual;
  try{actual=sha(f)}catch(e){if(e.code==="ENOENT"){console.log("SKIP (missing):",e.path);continue}throw e}
  if(e.content_sha256!==actual){
    console.log("FIX:",e.path);
    e.content_sha256=actual;
    fixed++;
  }
}
if(fixed){
  const tmp=mf+".tmp-"+process.pid;
  fs.writeFileSync(tmp,JSON.stringify(m,null,2)+"\n");
  fs.renameSync(tmp,mf);
  console.log("Fixed "+fixed+" manifest hash(es)");
}else{console.log("All hashes up to date");}
'
