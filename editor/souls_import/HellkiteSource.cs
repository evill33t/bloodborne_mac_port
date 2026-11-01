// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;

// Explicit DS1R encounter inputs. Source event bytes are checked before native
// controllers are emitted; source opcodes are never executed as native code.
internal static class HellkiteSource {
    internal sealed record Motion(int SourceID,string SHA256,float Duration,Vector4[] Samples) {
        internal Vector4 At(float fraction) {
            float frame=Math.Clamp(fraction,0,1)*(Samples.Length-1);int lo=(int)frame,hi=Math.Min(lo+1,Samples.Length-1);
            return Vector4.Lerp(Samples[lo],Samples[hi],frame-lo);
        }
    }
    internal sealed record Plan(MSB1.Part.Enemy Bridge,MSB1.Part.Enemy Flyby,MSB1.Region FlybyTrigger,MSB1.Region FlybyStart,
        MSB1.Region BridgeTrigger,MSB1.Region BridgeExclusion,MSB1.Region BridgeStart,MSB1.Region Perch,IReadOnlyDictionary<int,Motion> Motions,
        IReadOnlyDictionary<long,string> SourceEvents);
    internal static void Require(bool value,string message){if(!value)throw new InvalidDataException("Hellkite: "+message);}
    static string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();
    static string Hash(byte[] bytes)=>Convert.ToHexStringLower(SHA256.HashData(bytes));
    internal static Plan Load(Func<string,byte[]?> read) {
        byte[] Read(string path)=>read(path)??throw new FileNotFoundException("Hellkite source dependency missing",path);
        var map=MSB1.Read(Read("map/MapStudio/m10_01_00_00.msb"));var script=EMEVD.Read(Read("event/m10_01_00_00.emevd.dcx"));
        Require(script.Format==EMEVD.Game.DarkSouls1,"unsupported source event VM");
        var contracts=new Dictionary<long,string>{[11010790]="11c9cd0d53632a4c53c0adbbd992dd1692f8ddb695930f9153cf3a58f21da85d",[11010791]="13318401a7cc3fdbba33d7addf83dea62dbbc7f738fd58025c67e3774be67c91"};
        foreach(var (id,sha) in contracts)Require(BossEncounterSource.Fingerprint(script.Events.Single(e=>e.ID==id))==sha,"source flyby/bridge event changed: "+id);
        var constructor=script.Events.Single(e=>e.ID==0);
        foreach(int id in new[]{11010790,11010791}) {
            var args=new[]{0,id,0}.SelectMany(BitConverter.GetBytes).ToArray();
            Require(constructor.Instructions.Count(i=>i.Bank==2000&&i.ID==0&&i.ArgData.AsSpan().SequenceEqual(args))==1,"source encounter is not initialized exactly once");
        }
        MSB1.Part.Enemy Actor(string name,int entity,int init,Vector3 position,Vector3 rotation,string collision) {
            var a=map.Parts.Enemies.Single(p=>p.Name==name);
            Require(a.ModelName=="c3430"&&a.EntityID==entity&&a.NPCParamID==343000&&a.ThinkParamID==343000&&a.CharaInitID==-1&&a.TalkID==0&&a.InitAnimID==init&&a.Position==position&&a.Rotation==rotation&&a.Scale==Vector3.One&&a.CollisionName==collision&&a.DrawGroups.All(g=>g==0)&&a.DispGroups.All(g=>g==0),"unsupported source actor: "+name);
            return a;
        }
        MSB1.Region Region(int entity) {
            var r=map.Regions.GetEntries().Single(p=>p.EntityID==entity);
            Require(float.IsFinite(r.Position.LengthSquared())&&float.IsFinite(r.Rotation.LengthSquared())&&r.Shape is MSB.Shape.Point or MSB.Shape.Box or MSB.Shape.Cylinder,"unsupported source region: "+entity);return r;
        }
        var animation=BND3.Read(Read("chr/c3430.anibnd.dcx"));
        (int ID,string Hash,float Duration,Vector4 End)[] clips=[
            (7012,"61c50da1127881babded2c69eda9a624f4266c5c0f9c0322aee38d7caa46b9ee",9.333333f,new(-101.71908f,42.63499f,-244.53955f,0)),
            (7005,"37427c0304b386c8409edb58c2848de2f7689461a9d1f1cd9eabafa10b3541ee",13.166667f,new(195.3284f,0,-253.16786f,0)),
            (7006,"d0a59a7b5ad94b9f85832b8029f46fdbf12447e41d7d8486f90757c89d849fee",3f,new(0,0,0,.0009765625f))];
        var motions=new Dictionary<int,Motion>();
        foreach(var c in clips) {
            var bytes=animation.Files.Single(f=>Leaf(f.Name)==$"a00_{c.ID:D4}.hkx").Bytes.ToArray();Require(Hash(bytes)==c.Hash,"authored flight clip changed: "+c.ID);
            var tag=new SourceCollision.Tag(bytes);var a=tag.Items.Single(i=>tag.Name(i.Type)=="hkaSplineCompressedAnimation");
            var frame=tag.Reference(tag.Nested(a.Type,a.At,"extractedMotion").At);Require(tag.Name(frame.Type)=="hkaDefaultAnimatedReferenceFrame","unsupported source reference frame");
            float F(int at)=>BitConverter.UInt32BitsToSingle(tag.UInt(at));
            Require(F(tag.Nested(a.Type,a.At,"duration").At)==c.Duration&&F(tag.Nested(frame.Type,frame.At,"duration").At)==c.Duration&&tag.Vector(tag.Nested(frame.Type,frame.At,"up").At)==Vector3.UnitY&&tag.Vector(tag.Nested(frame.Type,frame.At,"forward").At)==Vector3.UnitZ,"source motion timing/basis changed");
            var samples=tag.Array(frame,"referenceFrameSamples");Require(samples.Count is >=2 and <=1024,"source flight sample budget exceeded");
            var values=Enumerable.Range(0,samples.Count).Select(i=>{int at=tag.Element(samples,i).At;return new Vector4(tag.Vector(at),F(at+12));}).ToArray();
            Require(values.All(v=>float.IsFinite(v.LengthSquared())&&v.Length()<1000)&&Vector4.Distance(values[^1],c.End)<.0001f,"invalid source flight trajectory");
            motions.Add(c.ID,new(c.ID,c.Hash,c.Duration,values));
        }
        return new(Actor("c3430_0000",1010300,7006,new(67.75f,15.051f,-343.313f),new(0,-150.973f,0),"h1115B1"),
            Actor("c3430_0001",1010302,-1,new(-128.602f,-46.348f,39.207f),new(0,-79.018f,0),"h1099B1"),
            Region(1012301),Region(1012300),Region(1012305),Region(1012337),Region(1012302),Region(1012310),motions,contracts);
    }
}
