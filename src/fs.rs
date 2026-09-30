//! FUSE glue: a read-only filesystem whose top level lists the configured
//! roots and whose directories come from `Library::list_dir`.

use crate::scan::{EntryKind, Library};
use crate::vfile::VFile;
use fuser::{
    Errno, FileAttr, FileHandle, FileType, FopenFlags, Filesystem, Generation, INodeNo, LockOwner, OpenFlags, ReplyAttr, ReplyData,
    ReplyDirectory, ReplyEntry, ReplyOpen, ReplyStatfs, ReplyXattr, Request,
};
use log::{debug, warn};
use parking_lot::RwLock;
use std::collections::HashMap;
use std::ffi::{OsStr, OsString};
use std::path::PathBuf;
use std::sync::Arc;
use std::time::{Duration, SystemTime};

const TTL: Duration = Duration::from_secs(1);
const XATTR_SOURCE: &str = "user.wavemorph.source";

#[derive(Clone)]
enum Kind {
    Root,
    Dir(PathBuf),
    File,
}

struct Node {
    parent: u64,
    name: OsString,
    kind: Kind,
}

#[derive(Default)]
struct Nodes {
    list: Vec<Node>,
    by_name: HashMap<(u64, OsString), u64>,
}

pub struct WaveFs {
    lib: Arc<Library>,
    nodes: RwLock<Nodes>,
    uid: u32,
    gid: u32,
    started: SystemTime,
}

impl WaveFs {
    pub fn new(lib: Arc<Library>) -> WaveFs {
        let mut nodes = Nodes::default();
        nodes.list.push(Node { parent: 1, name: OsString::new(), kind: Kind::Root });
        WaveFs {
            lib,
            nodes: RwLock::new(nodes),
            uid: unsafe { libc::getuid() },
            gid: unsafe { libc::getgid() },
            started: SystemTime::now(),
        }
    }

    fn node_kind(&self, ino: u64) -> Option<(u64, OsString, Kind)> {
        let n = self.nodes.read();
        n.list.get(ino as usize - 1).map(|x| (x.parent, x.name.clone(), x.kind.clone()))
    }

    fn ino_for(&self, parent: u64, name: &OsStr, kind: Kind) -> u64 {
        let key = (parent, name.to_os_string());
        // copy out first: holding the read guard while taking the write lock deadlocks
        let existing = self.nodes.read().by_name.get(&key).copied();
        if let Some(i) = existing {
            // refresh the directory source path in case it changed
            if let Kind::Dir(_) = kind {
                self.nodes.write().list[i as usize - 1].kind = kind;
            }
            return i;
        }
        let mut n = self.nodes.write();
        if let Some(&i) = n.by_name.get(&key) {
            return i;
        }
        n.list.push(Node { parent, name: name.to_os_string(), kind });
        let ino = n.list.len() as u64;
        n.by_name.insert(key, ino);
        ino
    }

    fn dir_attr(&self, ino: u64, mtime: SystemTime) -> FileAttr {
        FileAttr {
            ino: INodeNo(ino),
            size: 4096,
            blocks: 8,
            atime: mtime,
            mtime,
            ctime: mtime,
            crtime: mtime,
            kind: FileType::Directory,
            perm: 0o555,
            nlink: 2,
            uid: self.uid,
            gid: self.gid,
            rdev: 0,
            blksize: 131072,
            flags: 0,
        }
    }

    fn file_attr(&self, ino: u64, size: u64, mtime: SystemTime) -> FileAttr {
        FileAttr {
            ino: INodeNo(ino),
            size,
            blocks: size.div_ceil(512),
            atime: mtime,
            mtime,
            ctime: mtime,
            crtime: mtime,
            kind: FileType::RegularFile,
            perm: 0o444,
            nlink: 1,
            uid: self.uid,
            gid: self.gid,
            rdev: 0,
            blksize: 131072,
            flags: 0,
        }
    }

    /// Attributes of any node, resolving files through their parent's current listing.
    fn attr(&self, ino: u64) -> Result<FileAttr, Errno> {
        let (parent, name, kind) = self.node_kind(ino).ok_or(Errno::ENOENT)?;
        match kind {
            Kind::Root => Ok(self.dir_attr(ino, self.started)),
            Kind::Dir(p) => {
                let l = self.lib.list_dir(&p).map_err(|_| Errno::ENOENT)?;
                Ok(self.dir_attr(ino, l.mtime))
            }
            Kind::File => {
                let (vf, mt) = self.resolve_file(parent, &name)?;
                Ok(self.file_attr(ino, vf.size(), mt))
            }
        }
    }

    fn resolve_file(&self, parent: u64, name: &OsStr) -> Result<(Arc<dyn VFile>, SystemTime), Errno> {
        let Some((_, _, Kind::Dir(p))) = self.node_kind(parent) else { return Err(Errno::ENOENT) };
        let l = self.lib.list_dir(&p).map_err(|_| Errno::ENOENT)?;
        match l.find(name).map(|e| (&e.kind, e.mtime)) {
            Some((EntryKind::File(vf), mt)) => Ok((Arc::clone(vf), mt)),
            _ => Err(Errno::ENOENT),
        }
    }

    /// Children of a directory node: (name, kind, is_dir)
    fn children(&self, ino: u64) -> Result<Vec<(OsString, Kind, bool)>, Errno> {
        match self.node_kind(ino).ok_or(Errno::ENOENT)?.2 {
            Kind::Root => Ok(self.lib.cfg.roots.iter().map(|r| (OsString::from(&r.name), Kind::Dir(r.path.clone()), true)).collect()),
            Kind::Dir(p) => {
                let l = self.lib.list_dir(&p).map_err(|e| {
                    warn!("list {}: {e:#}", p.display());
                    Errno::EIO
                })?;
                Ok(l.entries
                    .iter()
                    .map(|e| match &e.kind {
                        EntryKind::Dir(d) => (e.name.clone(), Kind::Dir(d.clone()), true),
                        EntryKind::File(_) => (e.name.clone(), Kind::File, false),
                    })
                    .collect())
            }
            Kind::File => Err(Errno::ENOTDIR),
        }
    }
}

impl Filesystem for WaveFs {
    fn lookup(&self, _req: &Request, parent: INodeNo, name: &OsStr, reply: ReplyEntry) {
        let res = (|| {
            let kids = self.children(parent.0)?;
            let (n, kind, _) = kids.into_iter().find(|(n, _, _)| n == name).ok_or(Errno::ENOENT)?;
            let ino = self.ino_for(parent.0, &n, kind);
            self.attr(ino)
        })();
        match res {
            Ok(a) => reply.entry(&TTL, &a, Generation(0)),
            Err(e) => reply.error(e),
        }
    }

    fn getattr(&self, _req: &Request, ino: INodeNo, _fh: Option<FileHandle>, reply: ReplyAttr) {
        match self.attr(ino.0) {
            Ok(a) => reply.attr(&TTL, &a),
            Err(e) => reply.error(e),
        }
    }

    fn open(&self, _req: &Request, ino: INodeNo, flags: OpenFlags, reply: ReplyOpen) {
        if flags.0 & libc::O_ACCMODE != libc::O_RDONLY {
            return reply.error(Errno::EROFS);
        }
        match self.node_kind(ino.0) {
            Some((_, _, Kind::File)) => reply.opened(FileHandle(0), FopenFlags::empty()),
            Some(_) => reply.error(Errno::EISDIR),
            None => reply.error(Errno::ENOENT),
        }
    }

    fn read(
        &self,
        _req: &Request,
        ino: INodeNo,
        _fh: FileHandle,
        offset: u64,
        size: u32,
        _flags: OpenFlags,
        _lock_owner: Option<LockOwner>,
        reply: ReplyData,
    ) {
        let Some((parent, name, Kind::File)) = self.node_kind(ino.0) else { return reply.error(Errno::ENOENT) };
        let vf = match self.resolve_file(parent, &name) {
            Ok((vf, _)) => vf,
            Err(e) => return reply.error(e),
        };
        match vf.read_at(offset, size as usize) {
            Ok(b) => reply.data(&b),
            Err(e) => {
                warn!("read {} @{offset}+{size}: {e:#}", vf.describe());
                reply.error(Errno::EIO)
            }
        }
    }

    fn readdir(&self, _req: &Request, ino: INodeNo, _fh: FileHandle, offset: u64, mut reply: ReplyDirectory) {
        let kids = match self.children(ino.0) {
            Ok(k) => k,
            Err(e) => return reply.error(e),
        };
        let parent = self.node_kind(ino.0).map_or(1, |n| n.0);
        let mut all: Vec<(u64, FileType, OsString)> = vec![(ino.0, FileType::Directory, ".".into()), (parent, FileType::Directory, "..".into())];
        for (n, kind, is_dir) in kids {
            let child = self.ino_for(ino.0, &n, kind);
            all.push((child, if is_dir { FileType::Directory } else { FileType::RegularFile }, n));
        }
        for (i, (child, ft, name)) in all.into_iter().enumerate().skip(offset as usize) {
            if reply.add(INodeNo(child), (i + 1) as u64, ft, &name) {
                break;
            }
        }
        reply.ok();
    }

    fn statfs(&self, _req: &Request, _ino: INodeNo, reply: ReplyStatfs) {
        let Some(root) = self.lib.cfg.roots.first() else { return reply.statfs(0, 0, 0, 0, 0, 4096, 255, 4096) };
        let c = std::ffi::CString::new(root.path.as_os_str().as_encoded_bytes()).unwrap_or_default();
        let mut s: libc::statvfs = unsafe { std::mem::zeroed() };
        if unsafe { libc::statvfs(c.as_ptr(), &mut s) } == 0 {
            reply.statfs(s.f_blocks, 0, 0, s.f_files, 0, s.f_bsize as u32, 255, s.f_frsize as u32);
        } else {
            reply.statfs(0, 0, 0, 0, 0, 4096, 255, 4096);
        }
    }

    fn getxattr(&self, _req: &Request, ino: INodeNo, name: &OsStr, size: u32, reply: ReplyXattr) {
        if name != XATTR_SOURCE {
            return reply.error(Errno::ENODATA);
        }
        let value = match self.node_kind(ino.0) {
            Some((parent, n, Kind::File)) => match self.resolve_file(parent, &n) {
                Ok((vf, _)) => vf.describe(),
                Err(e) => return reply.error(e),
            },
            Some((_, _, Kind::Dir(p))) => format!("dir:{}", p.display()),
            _ => return reply.error(Errno::ENODATA),
        };
        debug!("xattr {value}");
        let b = value.into_bytes();
        if size == 0 {
            reply.size(b.len() as u32);
        } else if (size as usize) < b.len() {
            reply.error(Errno::ERANGE);
        } else {
            reply.data(&b);
        }
    }

    fn listxattr(&self, _req: &Request, _ino: INodeNo, size: u32, reply: ReplyXattr) {
        let mut b = XATTR_SOURCE.as_bytes().to_vec();
        b.push(0);
        if size == 0 {
            reply.size(b.len() as u32);
        } else if (size as usize) < b.len() {
            reply.error(Errno::ERANGE);
        } else {
            reply.data(&b);
        }
    }
}
