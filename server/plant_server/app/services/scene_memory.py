"""Private per-owner visual landmarks; physical poses, reference images and history."""
import base64
import hashlib
import io
import json
import math
import os
import re
import sqlite3
import threading
import time
import uuid
from pathlib import Path

import cv2
import numpy as np


class SceneMemory:
    def __init__(self, root, model, describe=None, compare=None):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self.dbpath = self.root / "memory.sqlite"
        self.model = str(model)
        self.describe = describe
        self.compare_images = compare
        self.lock = threading.RLock()
        self.face_lock = threading.Lock()
        self.detector = None
        with self.connect() as con:
            con.executescript("""
            CREATE TABLE IF NOT EXISTS scenes(owner TEXT PRIMARY KEY, generation INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS targets(id TEXT PRIMARY KEY, owner TEXT NOT NULL,
              scene INTEGER NOT NULL, name TEXT NOT NULL DEFAULT '', kind TEXT NOT NULL,
              label TEXT NOT NULL, yaw INTEGER NOT NULL, pitch INTEGER NOT NULL,
              bbox TEXT NOT NULL, reference TEXT NOT NULL, seen REAL NOT NULL);
            CREATE INDEX IF NOT EXISTS target_owner ON targets(owner,scene,name);
            CREATE TABLE IF NOT EXISTS observations(id INTEGER PRIMARY KEY AUTOINCREMENT, owner TEXT NOT NULL, yaw INTEGER NOT NULL, pitch INTEGER NOT NULL, path TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS photos(id TEXT PRIMARY KEY, target TEXT NOT NULL,
              owner TEXT NOT NULL, path TEXT NOT NULL, created REAL NOT NULL);
            """)
            columns={r[1] for r in con.execute("PRAGMA table_info(scenes)")}
            for key in ("horizontal_sign","vertical_sign"):
                if key not in columns:con.execute("ALTER TABLE scenes ADD COLUMN "+key+" INTEGER NOT NULL DEFAULT 0")

    def connect(self):
        con = sqlite3.connect(self.dbpath, timeout=10)
        con.row_factory = sqlite3.Row
        return con

    def scene(self, con, owner):
        con.execute("INSERT OR IGNORE INTO scenes(owner,generation) VALUES(?,1)", (owner,))
        return con.execute("SELECT generation FROM scenes WHERE owner=?", (owner,)).fetchone()[0]

    def reset(self, owner):
        with self.lock, self.connect() as con:
            generation = self.scene(con, owner) + 1
            con.execute("UPDATE scenes SET generation=?,horizontal_sign=0,vertical_sign=0 WHERE owner=?", (generation, owner))
            for row in con.execute("SELECT path FROM observations WHERE owner=?",(owner,)).fetchall():
                Path(row["path"]).unlink(missing_ok=True)
            con.execute("DELETE FROM observations WHERE owner=?",(owner,))
        return {"ok": True, "scene": generation, "message": "好，原来的方向记忆已失效，我们重新探索。"}

    @staticmethod
    def decode(data, fmt, width, height):
        if fmt == "raw565":
            if (width, height) not in {(160, 120), (320, 240)} or len(data) != width * height * 2:
                raise ValueError("invalid RGB565 dimensions or length")
            p = np.frombuffer(data, dtype="<u2").reshape(height, width)
            return np.stack(((p & 31) * 255 // 31, ((p >> 5) & 63) * 255 // 63,
                             (p >> 11) * 255 // 31), axis=2).astype(np.uint8)
        if fmt != "jpeg" or len(data) > 512000:
            raise ValueError("invalid image format or size")
        # Read dimensions before decoding to reject compressed oversized images.
        from PIL import Image
        with Image.open(io.BytesIO(data)) as im:
            if im.width > 1280 or im.height > 1280:
                raise ValueError("image too large")
        image = cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_COLOR)
        if image is None:
            raise ValueError("invalid JPEG")
        return image

    @staticmethod
    def jpeg(image):
        ok, encoded = cv2.imencode(".jpg", image, [cv2.IMWRITE_JPEG_QUALITY, 90])
        if not ok:
            raise ValueError("JPEG encode failed")
        return encoded.tobytes()

    @staticmethod
    def crop(image, bbox):
        h, w = image.shape[:2]
        x, y, bw, bh = bbox
        a, b = max(0, int(x * w / 1000)), max(0, int(y * h / 1000))
        c, d = min(w, int((x + bw) * w / 1000)), min(h, int((y + bh) * h / 1000))
        if c - a < 12 or d - b < 12:
            raise ValueError("target box too small")
        return image[b:d, a:c].copy()

    def faces(self, image):
        with self.face_lock:
            if self.detector is None:
                if not os.path.isfile(self.model):
                    raise RuntimeError("face model unavailable")
                self.detector = cv2.FaceDetectorYN.create(self.model, "", (320, 320), 0.7, 0.3, 100)
            h, w = image.shape[:2]
            # OpenCV 4.x model uses a padded multiple-of-32 input.
            dw, dh = ((w + 31) // 32) * 32, ((h + 31) // 32) * 32
            padded = cv2.copyMakeBorder(image, 0, dh-h, 0, dw-w, cv2.BORDER_CONSTANT)
            self.detector.setInputSize((dw, dh))
            _, rows = self.detector.detect(padded)
        out = []
        for r in ([] if rows is None else rows):
            x,y,bw,bh = map(float,r[:4])
            x,y=max(0,x),max(0,y)
            bw,bh=min(bw,w-x),min(bh,h-y)
            if bw<10 or bh<10: continue
            box=[round(x*1000/w),round(y*1000/h),round(bw*1000/w),round(bh*1000/h)]
            out.append({"kind":"face","label":"人脸","bbox":box,"confidence":int(r[-1]*100)})
        return sorted(out,key=lambda o:abs(o["bbox"][0]+o["bbox"][2]/2-500))

    @staticmethod
    def similarity(reference, current):
        a=cv2.resize(reference,(256,256));b=cv2.resize(current,(256,256))
        ga=cv2.cvtColor(a,cv2.COLOR_BGR2GRAY);gb=cv2.cvtColor(b,cv2.COLOR_BGR2GRAY)
        if min(float(ga.std()),float(gb.std()))<8: return 0
        # Near-identical textured pictures cover repeated fixed-pose captures.
        if float(np.mean(np.abs(ga.astype(float)-gb.astype(float))))<3: return 99
        orb=cv2.ORB_create(nfeatures=600,edgeThreshold=12,fastThreshold=10)
        ka,da=orb.detectAndCompute(ga,None);kb,db=orb.detectAndCompute(gb,None)
        if da is None or db is None or len(ka)<12 or len(kb)<12:return 0
        pairs=cv2.BFMatcher(cv2.NORM_HAMMING).knnMatch(da,db,k=2)
        good=[p[0] for p in pairs if len(p)==2 and p[0].distance<.72*p[1].distance]
        if len(good)<12:return 0
        src=np.float32([ka[m.queryIdx].pt for m in good]);dst=np.float32([kb[m.trainIdx].pt for m in good])
        matrix,mask=cv2.findHomography(src,dst,cv2.RANSAC,4)
        if matrix is None or mask is None:return 0
        inliers=int(mask.sum())
        if inliers<12 or inliers/len(good)<.65:return 0
        return min(99,75+inliers)

    def observe(self, owner, image, yaw, pitch, frame_id, semantic=False):
        started=time.monotonic()
        with self.connect() as con: initial_scene=self.scene(con,owner)
        if semantic:self.save_observation(owner,image,yaw,pitch)
        with self.connect() as con:
            row=con.execute("SELECT horizontal_sign,vertical_sign FROM scenes WHERE owner=?",(owner,)).fetchone()
            calibration=dict(row)
        objects=self.faces(image)
        if semantic and self.describe:
            for obj in self.describe(self.jpeg(image)):
                if obj.get("kind") not in {"plant","object"}:continue
                box=obj.get("bbox",[])
                if len(box)!=4 or any(not isinstance(v,(int,float)) or not math.isfinite(v) for v in box):continue
                box=[int(v) for v in box]
                if min(box)<0 or box[0]+box[2]>1000 or box[1]+box[3]>1000 or min(box[2:])<40:continue
                if not 75<=int(obj.get("confidence",0))<=100:continue
                label=str(obj.get("label","")).strip()[:24]
                if not label:continue
                objects.append({"kind":obj["kind"],"label":label,"bbox":box,"confidence":int(obj["confidence"])})
        usable=[]
        for obj in objects:
            try:self.crop(image,obj["bbox"])
            except ValueError:continue
            usable.append(obj)
        objects=usable
        result={**calibration,"ok":True,"frame_id":frame_id,"found":False,"count":len(objects),"message":"还没发现清晰的目标。"}
        if not objects:return result
        # A plant is the first exploration landmark; face-only mode stays anonymous.
        chosen=next((o for o in objects if o["kind"]=="plant"),objects[0])
        box=chosen["bbox"]
        result.update(chosen,found=True,cx=box[0]+box[2]//2,cy=box[1]+box[3]//2)
        if semantic:
            crop=self.crop(image,box)
            with self.lock,self.connect() as con:
                scene=self.scene(con,owner)
                if scene!=initial_scene:raise ValueError("场景已更新，请重新探索")
                rows=con.execute("SELECT * FROM targets WHERE owner=? AND scene=? AND kind=?",(owner,scene,chosen["kind"])).fetchall()
                target=None
                for row in rows:
                    if abs(row["yaw"]-yaw)>48 or abs(row["pitch"]-pitch)>48:continue
                    ref=cv2.imread(row["reference"])
                    if ref is not None and self.similarity(ref,crop)>=85:target=row;break
                tid=target["id"] if target else uuid.uuid4().hex[:20]
                directory=self.root/hashlib.sha256(owner.encode()).hexdigest()[:24]
                directory.mkdir(exist_ok=True)
                path=directory/(tid+"_ref.jpg")
                if target is None:
                    path.write_bytes(self.jpeg(crop))
                    con.execute("INSERT INTO targets VALUES(?,?,?,?,?,?,?,?,?,?,?)",(tid,owner,scene,"",chosen["kind"],chosen["label"],yaw,pitch,json.dumps(box),str(path),time.time()))
                else:
                    # Refine observation pose only when the target is reasonably centered.
                    centered=abs(result["cx"]-500)<=150 and abs(result["cy"]-500)<=200
                    con.execute("UPDATE targets SET seen=?,yaw=?,pitch=?,bbox=? WHERE id=?",(time.time(),yaw if centered else target["yaw"],pitch if centered else target["pitch"],json.dumps(box),tid))
                result.update(target_id=tid,name=target["name"] if target else "",yaw=yaw,pitch=pitch)
        result["message"]="发现了%s。"%chosen["label"]
        result["latency_ms"]=int((time.monotonic()-started)*1000)
        return result

    @staticmethod
    def image_translation(a,b):
        a=cv2.cvtColor(cv2.resize(a,(320,240)),cv2.COLOR_BGR2GRAY)
        b=cv2.cvtColor(cv2.resize(b,(320,240)),cv2.COLOR_BGR2GRAY)
        if min(float(a.std()),float(b.std()))<8:return None
        sift=cv2.SIFT_create(nfeatures=600)
        ka,da=sift.detectAndCompute(a,None);kb,db=sift.detectAndCompute(b,None)
        pairs=[] if da is None or db is None else cv2.BFMatcher().knnMatch(da,db,k=2)
        good=[p[0] for p in pairs if len(p)==2 and p[0].distance<.68*p[1].distance]
        if len(good)>=12:
            src=np.float32([ka[m.queryIdx].pt for m in good]);dst=np.float32([kb[m.trainIdx].pt for m in good])
            matrix,mask=cv2.estimateAffinePartial2D(src,dst,method=cv2.RANSAC,ransacReprojThreshold=3)
            if matrix is not None and mask is not None and int(mask.sum())>=12:
                displacement=(dst-src)[mask.ravel().astype(bool)]
                return np.median(displacement,axis=0)
        # Low-texture landmarks can lack SIFT corners. Accept only a strong
        # whole-image translation; calibration still needs two agreeing poses.
        window=cv2.createHanningWindow((320,240),cv2.CV_32F)
        shift,response=cv2.phaseCorrelate(a.astype(np.float32),b.astype(np.float32),window)
        if response<.65 or abs(shift[0])>120 or abs(shift[1])>90:return None
        return np.asarray(shift)

    def save_observation(self,owner,image,yaw,pitch):
        directory=self.root/hashlib.sha256(owner.encode()).hexdigest()[:24]
        directory.mkdir(exist_ok=True)
        path=directory/("obs_"+uuid.uuid4().hex[:16]+".jpg")
        path.write_bytes(self.jpeg(image))
        with self.lock,self.connect() as con:
            self.scene(con,owner)
            rows=con.execute("SELECT * FROM observations WHERE owner=? ORDER BY id DESC LIMIT 12",(owner,)).fetchall()
            votes=[[],[]]
            for row in rows:
                dyaw=yaw-row["yaw"];dpitch=pitch-row["pitch"]
                axis=0 if 8<=abs(dyaw)<=48 and abs(dyaw)>=2*abs(dpitch) else 1 if 8<=abs(dpitch)<=48 and abs(dpitch)>=2*abs(dyaw) else None
                if axis is None:continue
                old=cv2.imread(row["path"])
                if old is None:continue
                shift=self.image_translation(old,image)
                if shift is None or abs(float(shift[axis]))<3:continue
                delta=dyaw if axis==0 else dpitch
                votes[axis].append(1 if shift[axis]*delta>0 else -1)
            for axis,key in enumerate(("horizontal_sign","vertical_sign")):
                if len(votes[axis])>=2 and len(set(votes[axis]))==1:
                    con.execute("UPDATE scenes SET "+key+"=? WHERE owner=?",(votes[axis][0],owner))
            con.execute("INSERT INTO observations(owner,yaw,pitch,path) VALUES(?,?,?,?)",(owner,yaw,pitch,str(path)))
            old=con.execute("SELECT id,path FROM observations WHERE owner=? ORDER BY id DESC LIMIT -1 OFFSET 12",(owner,)).fetchall()
            for row in old:
                con.execute("DELETE FROM observations WHERE id=?",(row["id"],))
                Path(row["path"]).unlink(missing_ok=True)

    def remember(self,owner,tid,name):
        if not re.fullmatch(r"[\u4e00-\u9fffA-Za-z0-9]{1,12}",name):raise ValueError("名字请用一到十二个汉字或字母数字")
        with self.lock,self.connect() as con:
            scene=self.scene(con,owner)
            target=con.execute("SELECT * FROM targets WHERE owner=? AND scene=? AND id=?",(owner,scene,tid)).fetchone()
            if target is None:raise LookupError("请先探索或对准要记住的目标")
            other=con.execute("SELECT id FROM targets WHERE owner=? AND scene=? AND name=? AND id!=?",(owner,scene,name,tid)).fetchone()
            if other:raise ValueError("这个名字已经用过，请换个名字")
            con.execute("UPDATE targets SET name=? WHERE id=?",(name,tid))
        return {"ok":True,"target_id":tid,"name":name,"message":"记住啦，它叫%s。"%name}

    def lookup(self,owner,name):
        with self.connect() as con:
            scene=self.scene(con,owner)
            row=con.execute("SELECT * FROM targets WHERE owner=? AND scene=? AND name=?",(owner,scene,name)).fetchone()
        if row is None:raise LookupError("我还没记住%s，请先带我认识它。"%name)
        return dict(row)

    def names(self,owner):
        with self.connect() as con:
            scene=self.scene(con,owner)
            return [r[0] for r in con.execute("SELECT name FROM targets WHERE owner=? AND scene=? AND name!=''",(owner,scene))]

    def verify(self,owner,name,image,frame_id,save=False,compare=False):
        target=self.lookup(owner,name)
        bbox=json.loads(target["bbox"])
        current=self.crop(image,bbox)
        reference=cv2.imread(target["reference"])
        score=0 if reference is None else self.similarity(reference,current)
        result={"ok":True,"frame_id":frame_id,"found":score>=85,"confidence":score,"name":name,"target_id":target["id"],"message":"还不能确认这是%s，请重新指给我看。"%name}
        if score<85:return result
        result["message"]="找到%s啦。"%name
        if save or compare:
            pid=uuid.uuid4().hex[:20];path=Path(target["reference"]).parent/(pid+"_photo.jpg")
            path.write_bytes(self.jpeg(image))
            with self.connect() as con:
                old=con.execute("SELECT path FROM photos WHERE owner=? AND target=? ORDER BY created DESC LIMIT 1",(owner,target["id"])).fetchone()
                con.execute("INSERT INTO photos VALUES(?,?,?,?,?)",(pid,target["id"],owner,str(path),time.time()))
            result.update(photo_id=pid,message="给%s拍好照片啦。"%name)
            if compare:
                if target["kind"]!="plant":result["message"]="这个目标还没有确认为植物，我们先记录照片。"
                elif old and self.compare_images:
                    result["message"]=self.compare_images(Path(old["path"]).read_bytes(),self.jpeg(image))[:240]
                else:result["message"]="已记录%s的第一张生长照片，下次就能比较啦。"%name
        return result
