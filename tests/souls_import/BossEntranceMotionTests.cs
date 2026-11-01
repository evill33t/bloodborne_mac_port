// SPDX-License-Identifier: GPL-3.0-or-later
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
using SoulsFormats;

static class BossEntranceMotionTests {
    static void Check(bool value,string why){if(!value)throw new Exception(why);}
    static void Reject(Action action){try{action();}catch(InvalidDataException){return;}throw new Exception("Unsupported entrance input accepted");}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static string Hash(byte[] b)=>Convert.ToHexStringLower(SHA256.HashData(b));
    // Independent raw fixup-table reader: a managed roundtrip would not prove
    // the new typed object has a native global pointer and virtual class fixup.
    sealed class Raw {
        readonly byte[] bytes;internal readonly int Data;internal readonly Dictionary<int,int> Local=[];
        internal readonly Dictionary<int,(int Section,int Target)> Global=[];
        internal readonly Dictionary<string,(int At,uint Signature)> Types=[];
        internal Raw(byte[] b) {
            bytes=b;int s=64+BitConverter.ToUInt16(b,62),h=s+128,classes=BitConverter.ToInt32(b,s+20);Data=BitConverter.ToInt32(b,h+20);
            int local=BitConverter.ToInt32(b,h+24),global=BitConverter.ToInt32(b,h+28),virt=BitConverter.ToInt32(b,h+32),end=BitConverter.ToInt32(b,h+36);
            for(int a=local;a+8<=global;a+=8)if(I(a)!=-1)Local.Add(I(a),I(a+4));
            for(int a=global;a+12<=virt;a+=12)if(I(a)!=-1)Global.Add(I(a),(I(a+4),I(a+8)));
            for(int a=virt;a+12<=end;a+=12)if(I(a)!=-1) {
                Check(I(a+4)==0,"Class fixup targets wrong section");int at=classes+I(a+8),stop=System.Array.IndexOf(b,(byte)0,at);
                Types.Add(System.Text.Encoding.ASCII.GetString(b,at,stop-at),(I(a),BitConverter.ToUInt32(b,at-5)));
            }
        }
        internal int I(int at)=>BitConverter.ToInt32(bytes,Data+at);
        internal float F(int at)=>BitConverter.ToSingle(bytes,Data+at);
        internal Vector4 V(int at)=>new(F(at),F(at+4),F(at+8),F(at+12));
        internal byte[] Array(int field,int stride)=>bytes.AsSpan(Data+Local[field],I(field+8)*stride).ToArray();
    }
    static string Animation(TAE.Animation a)=>JsonSerializer.Serialize(new{a.ID,a.AnimFileName,Header=a.MiniHeader.Type,Events=a.Events.Select(e=>new{e.Type,e.Unk04,e.StartTime,e.EndTime,Bytes=Convert.ToHexString(e.GetParameterBytes(false)),Group=e.Group?.GroupType})});
    static void Main(string[] args) {
        if(args.Length!=4)throw new ArgumentException("Supply DS1R root, native dvdroot, pre-entrance Taurus anibnd and artifact output");
        var hashes=new Dictionary<string,string>();byte[] Read(string path){var b=File.ReadAllBytes(path);hashes[path]=Hash(b);return b;}
        byte[]? Source(string relative){string p=Path.Combine(args[0],relative);return File.Exists(p)?Read(p):null;}
        byte[] native=Read(Path.Combine(args[1],"chr/c3130.anibnd.dcx")),input=Read(args[2]);
        var before=BND4.Read(input);var result=BossEntranceMotion.Apply(Source,input,native);var after=BND4.Read(result.Bytes);
        Check(before.Files.Count==after.Files.Count&&before.Files.Select(f=>(f.ID,f.Name,f.Flags)).SequenceEqual(after.Files.Select(f=>(f.ID,f.Name,f.Flags))),"Binder records/order changed");
        int preserved=0;foreach(var f in before.Files)if(f.ID is not(1000007000 or 1000007010 or 3000000)){Check(f.Bytes.Span.SequenceEqual(after.Files.Single(x=>x.ID==f.ID).Bytes.Span),"Unrelated animation/behavior/skeleton changed");preserved++;}
        var sourceBank=BND3.Read(Source("chr/c2250.anibnd.dcx")!);var sourceJump=sourceBank.Files.Single(f=>Leaf(f.Name)=="a00_9061.hkx");
        var tag=new SourceCollision.Tag(sourceJump.Bytes.ToArray());var a=tag.Items.Single(i=>tag.Name(i.Type)=="hkaSplineCompressedAnimation");var frame=tag.Reference(tag.Nested(a.Type,a.At,"extractedMotion").At);var samples=tag.Array(frame,"referenceFrameSamples");
        Check(samples.Count==126&&result.Report.Samples==126&&result.Report.Duration==4.1666665f,"Source timing changed");
        var donor=BND4.Read(native);var originalFrame=new Raw(donor.Files.Single(f=>f.ID==1000007000).Bytes.ToArray());
        const string type="hkaDefaultAnimatedReferenceFrame";int sourceFrames=0;
        foreach(int id in new[]{1000007000,1000007010}) {
            var old=new Raw(before.Files.Single(f=>f.ID==id).Bytes.ToArray());var now=new Raw(after.Files.Single(f=>f.ID==id).Bytes.ToArray());
            int animation=now.Types["hkaSplineCompressedAnimation"].At,r=now.Types[type].At;
            Check(now.Global[animation+32]==(2,r)&&!now.Local.ContainsKey(animation+32),"Native extracted-motion typed global fixup missing");
            Check(now.Types[type].Signature==originalFrame.Types[type].Signature,"New class signature differs from retail");
            Check(now.V(r+16)==new Vector4(0,1,0,0)&&now.V(r+32)==new Vector4(0,0,1,0),"Root-motion coordinate basis changed");
            foreach(var (offset,stride) in new[]{(88,4),(104,4),(120,4),(136,4),(152,1)}) {
                if(old.I(animation+offset+8)==0){Check(now.I(animation+offset+8)==0,"Empty array acquired content");continue;}
                Check(old.Array(animation+offset,stride).SequenceEqual(now.Array(animation+offset,stride)),"Pose/channel/block payload changed");
            }
            Check(old.I(animation+20)==now.I(animation+20)&&old.I(animation+56)==now.I(animation+56)&&old.I(animation+60)==now.I(animation+60),"Track/frame/block count changed");
            if(id==1000007000) {
                Check(now.F(animation+16)==old.F(animation+16)&&now.F(r+48)==old.F(animation+16)&&now.I(r+64)==2,"Stationary wait loop retimed");
                Check(now.V(now.Local[r+56])==Vector4.Zero&&now.V(now.Local[r+56]+16)==Vector4.Zero,"Waiting still has physical drift");
                Check(old.Array(old.Types[type].At+56,16).Any(b=>b!=0),"Fixture no longer reproduces unwanted donor wait movement");
            } else {
                Check(!old.Types.ContainsKey(type),"Fixture no longer reproduces missing jump motion");
                Check(now.F(animation+16)==result.Report.Duration&&now.F(r+48)==result.Report.Duration&&now.I(r+64)==126,"Jump timing/reference-frame length mismatch");
                Check(Math.Abs(now.F(animation+80)*130-result.Report.Duration)<.000001f&&Math.Abs(now.F(animation+72)*now.F(animation+76)-1)<.000001f,"Frame/block timing disagrees");
                for(int i=0;i<126;i++){int at=tag.Element(samples,i).At;var expected=new Vector4(tag.Vector(at),BitConverter.Int32BitsToSingle(tag.Int(at+12)));Check(now.V(now.Local[r+56]+i*16)==expected,"Authored source trajectory sample changed");sourceFrames++;}
                var end=now.V(now.Local[r+56]+125*16);var world=new Vector3(-6.7f,26.98f,-110.4f)+Vector3.Transform(new(end.X,end.Y,end.Z),Matrix4x4.CreateRotationY(-65*MathF.PI/180));
                Check(Vector3.Distance(world,new(2.4245462f,15.519776f,-114.402176f))<.00001f,"Physical trajectory endpoint disagrees with source placement");
            }
        }
        var sourceTae=TAE.Read(sourceBank.Files.Single(f=>Leaf(f.Name).EndsWith(".tae",StringComparison.OrdinalIgnoreCase)).Bytes);
        var oldTae=TAE.Read(before.Files.Single(f=>f.ID==3000000).Bytes);var newTae=TAE.Read(after.Files.Single(f=>f.ID==3000000).Bytes);
        Check(oldTae.ID==newTae.ID&&oldTae.Format==newTae.Format&&oldTae.Animations.Select(a=>a.ID).SequenceEqual(newTae.Animations.Select(a=>a.ID)),"TAE global identity/order changed");
        foreach(var animation in oldTae.Animations.Where(a=>a.ID is not(7000 or 7010)))Check(Animation(animation)==Animation(newTae.Animations.Single(a=>a.ID==animation.ID)),"Unrelated TAE animation changed");
        foreach(var (sourceID,targetID) in new[]{(9001,7000),(9061,7010)}) {
            var expected=sourceTae.Animations.Single(a=>a.ID==sourceID).Events.Where(e=>e.Type==0).ToArray();var actual=newTae.Animations.Single(a=>a.ID==targetID).Events;
            Check(actual.Count==6&&actual.All(e=>e.Type==0),"Intro retains donor attacks/SFX or unsupported source events");
            for(int i=0;i<6;i++){var x=actual[i].GetParameterBytes(false);var y=expected[i].GetParameterBytes(false);Check(x.AsSpan(0,12).SequenceEqual(y)&&x.AsSpan(12).ToArray().All(b=>b==0)&&actual[i].StartTime==0,"Source movement flags or native parameter padding changed");Check(actual[i].EndTime==(targetID==7000?3.6666667f:Math.Min(expected[i].EndTime,4.1666665f)),"Gravity/collision state timing changed");}
        }
        Reject(()=>BossEntranceMotion.Apply(Source,result.Bytes,native));
        var bad=native.ToArray();bad[^1]^=1;Reject(()=>BossEntranceMotion.Apply(Source,input,bad));
        var corrupt=BND3.Read(Source("chr/c2250.anibnd.dcx")!);var changed=corrupt.Files.Single(f=>Leaf(f.Name)=="a00_9061.hkx");var broken=changed.Bytes.ToArray();broken[^1]^=1;changed.Bytes=broken;
        Reject(()=>BossEntranceMotion.Apply(p=>p=="chr/c2250.anibnd.dcx"?corrupt.Write():Source(p),input,native));
        foreach(var (path,hash) in hashes)Check(Hash(File.ReadAllBytes(path))==hash,"Input asset modified");
        Directory.CreateDirectory(args[3]);File.WriteAllBytes(Path.Combine(args[3],"c8950.anibnd.dcx"),result.Bytes);
        File.WriteAllText(Path.Combine(args[3],"tests.json"),JsonSerializer.Serialize(new{result.Report,SourceMotionSamples=sourceFrames,PreservedBinderEntries=preserved,PreservedOtherTaeAnimations=oldTae.Animations.Count-2,Guards=3,Inputs=hashes,Limit="Installed-asset format/readback proof only. Native controller/root-motion/collision landing still needs gameplay QA."},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));
        Console.WriteLine($"PASS Taurus entrance:126exactsource motion samples,stationarywait,12movementflags,{preserved}unrelatedbinderentries,{oldTae.Animations.Count-2}otherTAEanimations,byte-exactcompressedcurves,3rejectionguards,{hashes.Count}preservedinputs");
    }
}
