// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

static class MapDeathBoundariesTests {
    static readonly Dictionary<string,string> Hashes=new(StringComparer.Ordinal);
    static void Check(bool ok,string why){if(!ok)throw new Exception(why);}
    static byte[] Read(string path){var bytes=File.ReadAllBytes(path);Hashes.TryAdd(path,Convert.ToHexString(SHA256.HashData(bytes)));return bytes;}
    static byte[]? Asset(string root,string path)=>File.Exists(Path.Combine(root,path))?Read(Path.Combine(root,path)):null;
    static void Reject(Action action){try{action();}catch(Exception e)when(e is InvalidDataException or FileNotFoundException or NotSupportedException){return;}throw new Exception("Malformed source/native death sensor accepted");}
    static MSBB Destination() {
        var map=new MSBB();map.Models.Collisions.Add(new(){Name="h060000"});
        map.Parts.Collisions.Add(new(){Name="existing_floor",ModelName="h060000",EntityID=123,HitFilterID=8,DisableStart=true});
        map.Models.Players.Add(new(){Name="c0000"});map.Parts.Players.Add(new(){Name="player",ModelName="c0000",Position=new(3,7,11)});
        return map;
    }
    static void Main(string[] args) {
        var reports=new List<object>();var offset=new Vector3(-165.5f,3.25f,217.125f);
        byte[]? Native(string path)=>Asset(args[1],path);
        foreach(var (map,hCount,lCount,filters) in new[]{("m10_02_00_00",98,98,new byte[]{13,15,13,14}),("m10_01_00_00",169,209,new byte[]{13,15,13,15})}) {
            byte[]? Source(string path)=>Asset(args[0],path);
            var source=MSB1.Read(Source($"map/MapStudio/{map}.msb")!);var sourceBytes=source.Write();
            var geometry=SourceCollision.Read("DS1R",map,Source,offset);
            var floors=geometry.Parts.Where(p=>p.PlayerSolid).SelectMany(p=>p.Surfaces).SelectMany(s=>s.Triangles).ToArray();
            var destination=Destination();var old=destination.Write();
            string target=map=="m10_02_00_00"?"m40_02_00_00":"m40_03_00_00";
            var result=MapDeathBoundaries.Apply("DS1R",source,geometry,destination,Native,target);
            Check(result.Bodies.Count==4&&result.Report.Boundaries.Count==4,"Source death-boundary inventory differs");
            Check(result.Report.Boundaries.Sum(b=>b.HighTriangles)==hCount&&result.Report.Boundaries.Sum(b=>b.LowTriangles)==lCount,"Source H/L death triangles were lost or synthesized");
            Check(result.Report.Boundaries.Select(b=>b.HitFilter).Order().SequenceEqual(filters.Order()),"Fall camera and instant-death filters were merged");
            Check(result.Bodies.Select(b=>b.Model).SequenceEqual(new[]{"h060001","h060002","h060003","h060004"}),"Model allocator overwrote an existing body");
            Check(source.Write().AsSpan().SequenceEqual(sourceBytes),"Source MSB was mutated");
            Check(floors.SequenceEqual(geometry.Parts.Where(p=>p.PlayerSolid).SelectMany(p=>p.Surfaces).SelectMany(s=>s.Triangles)),"Death import changed player-solid geometry");
            var decoded=MSBB.Read(destination.Write());Check(decoded.Parts.Collisions.Count==5,"Native MSB omitted staged fall sensors");
            foreach(var body in result.Bodies) {
                var binding=result.Report.Boundaries.Single(b=>b.TargetModel==body.Model);
                var part=decoded.Parts.Collisions.Single(p=>p.Name==body.Part.Name);
                Check(decoded.Models.Collisions.Single(m=>m.Name==body.Model).SibPath==@"N:\SPRJ\data\Model\map\"+target+@"\hkt\"+body.Model+"A40.hkt","Owned sensor retained donor map/model metadata");
                Check(part.ModelName==body.Model&&part.EntityID==-1&&part.HitFilterID==binding.HitFilter&&part.DisableStart==!binding.InitialEnabled,"Native sensor identity/filter/initial state did not round-trip");
                Check(part.MapNameID==-1&&part.PlayRegionID==-1&&part.DrawGroups.All(v=>v==0)&&part.DispGroups.All(v=>v==0)&&part.BackreadGroups.All(v=>v==0),"Fall sensor acquired floor visibility or area-name behavior");
                Check(part.Position==Vector3.Zero&&part.Rotation==Vector3.Zero&&part.Scale==Vector3.One,"Source placement was applied twice");
                foreach(var (layer,bytes) in new[]{("h",body.High),("l",body.Low)}) {
                    var authored=geometry.Parts.Single(p=>p.Name==binding.SourceName&&p.Layer==layer).Surfaces.SelectMany(s=>s.Triangles).ToArray();
                    var actual=Decode(bytes,layer+body.Model[1..]);
                    Check(actual.Triangles.Count==authored.Length,"Native sensor contains an invented plane or missing source triangle");
                    var unmatched=authored.ToList();float maximum=0;
                    foreach(var triangle in actual.Triangles) {
                        float Error(MapCollision.Triangle t)=>Math.Max(Vector3.Distance(t.A,triangle.T.A),Math.Max(Vector3.Distance(t.B,triangle.T.B),Vector3.Distance(t.C,triangle.T.C)));
                        int best=Enumerable.Range(0,unmatched.Count).MinBy(i=>Error(unmatched[i]));float error=Error(unmatched[best]);
                        Check(error<=triangle.Tolerance+.0002f,"Decoded sensor differs from authored source geometry/winding beyond its packing grid");
                        maximum=Math.Max(maximum,error);unmatched.RemoveAt(best);
                    }
                    Check(unmatched.Count==0,"Source geometry matching was incomplete");
                    reports.Add(new{map,binding.SourceName,binding.HitFilter,layer,triangles=authored.Length,maximumVertexError=maximum,bodySha=Convert.ToHexString(SHA256.HashData(bytes))});
                }
            }
            var mirror=Destination();var originalMirror=mirror.Write();MapDeathBoundaries.Mirror(mirror,result);
            Check(MSBB.Read(mirror.Write()).Parts.Collisions.Count==5,"Story variant lacks imported death sensors");
            foreach(var body in result.Bodies){destination.Parts.Collisions.Remove(body.Part);destination.Models.Collisions.Remove(body.Registration);mirror.Parts.Collisions.RemoveAll(p=>p.Name==body.Part.Name);mirror.Models.Collisions.RemoveAll(m=>m.Name==body.Model);}
            Check(destination.Write().AsSpan().SequenceEqual(old)&&mirror.Write().AsSpan().SequenceEqual(originalMirror),"Stage altered existing floors, players, flags, or registrations");
            if(map=="m10_02_00_00")Guards(source,geometry,Native);
        }
        foreach(var (path,hash) in Hashes)Check(Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)))==hash,"Source/native asset changed: "+path);
        Directory.CreateDirectory(Path.GetDirectoryName(args[2])!);File.WriteAllText(args[2],JsonSerializer.Serialize(new{reports,hashes=Hashes,limits="Format/geometry proof only; actual fall camera/death/respawn behavior requires native gameplay."},new JsonSerializerOptions{WriteIndented=true}));
        Console.WriteLine($"PASS: 8 authored fall sensors; 574 H/L triangles, native filter/body/MSB readback, geometry/namespace preservation, atomic rejection; {Hashes.Count} original assets hash-preserved");
    }

    static void Guards(MSB1 source,SourceCollision.Result geometry,Func<string,byte[]?> readNative) {
        var empty=Destination();var emptyBefore=empty.Write();var emptyResult=MapDeathBoundaries.Apply("DS1R",new MSB1(),new([]),empty,_=>throw new Exception("Empty death-boundary stage read native assets"));
        Check(emptyResult.Bodies.Count==0&&empty.Write().AsSpan().SequenceEqual(emptyBefore),"Empty stage synthesized a global death plane");
        void Atomic(SourceCollision.Result g,Func<string,byte[]?> read,string profile="DS1R",string? targetMap=null) {var map=Destination();var old=map.Write();Reject(()=>MapDeathBoundaries.Apply(profile,source,g,map,read,targetMap));Check(map.Write().AsSpan().SequenceEqual(old),"Rejected conversion partially mutated destination");}
        Atomic(geometry,_=>throw new Exception("Unsupported profile read native assets"),"DS1");
        foreach(string bad in new[]{"","../m40_02_00_00","m40_02_00_0x","m40a02_00_00"})Atomic(geometry,_=>throw new Exception("Invalid target map read native assets"),targetMap:bad);
        Atomic(new(geometry.Parts.Where(p=>!(p.Name=="h8100B2_0000"&&p.Layer=="l")).ToArray()),readNative);
        Atomic(new(geometry.Parts.Select(p=>p.Name=="h8100B2_0000"&&p.Layer=="l"?p with{HitFilter=15}:p).ToArray()),readNative);
        Atomic(new(geometry.Parts.Select(p=>p.Name=="h8100B2_0000"&&p.Layer=="l"?p with{Surfaces=[]}:p).ToArray()),readNative);
        Atomic(new(geometry.Parts.Select(p=>p.Name=="h8100B2_0000"&&p.Layer=="l"?p with{Surfaces=[new(0,[new(new(float.NaN,0,0),Vector3.UnitX,Vector3.UnitY)])]}:p).ToArray()),readNative);
        Atomic(geometry,path=>path.Contains("m24_02_00_00")?null:readNative(path));
        var exhausted=Destination();for(int i=60001;i<=65535;i++)exhausted.Models.Collisions.Add(new(){Name="h"+i.ToString("D6")});var before=exhausted.Write();
        Reject(()=>MapDeathBoundaries.Apply("DS1R",source,geometry,exhausted,_=>throw new Exception("Exhausted namespace read native assets")));
        Check(before.AsSpan().SequenceEqual(exhausted.Write()),"Exhausted collision namespace partially mutated scene");
        var duplicate=Destination();var valid=MapDeathBoundaries.Apply("DS1R",source,geometry,duplicate,readNative,"m24_01_00_11");before=duplicate.Write();
        Check(valid.Bodies.All(b=>b.Registration.SibPath==@"N:\SPRJ\data\Model\map\m24_01_00_00\hkt\"+b.Model+"A24.hkt"),"Story variant metadata did not use the native base map and destination area");
        Reject(()=>MapDeathBoundaries.Mirror(duplicate,valid));Check(before.AsSpan().SequenceEqual(duplicate.Write()),"Variant name conflict partially mutated scene");
    }

    // Independent byte-level packfile decoder. No exporter tree/quantization
    // routines are called to verify positions, index order, or physical flags.
    sealed record Decoded(List<(MapCollision.Triangle T,float Tolerance)> Triangles);
    static Decoded Decode(byte[] input,string bodyName) {
        byte[] b=DCX.Decompress(input).ToArray();uint U(int at)=>BitConverter.ToUInt32(b,at);
        int section=BitConverter.ToInt16(b,62)+64,names=(int)U(section+20),header=section+128,data=(int)U(header+20);
        int local=(int)U(header+24),global=(int)U(header+28),virt=(int)U(header+32),end=(int)U(header+36);
        var fix=new Dictionary<int,int>();for(int at=data+local;at<data+global;at+=8)if(U(at)!=uint.MaxValue)fix.Add((int)U(at),(int)U(at+4));
        var classes=new Dictionary<string,int>();for(int at=data+virt;at<data+end;at+=12)if(U(at)!=uint.MaxValue){int name=names+(int)U(at+8);classes.Add(Encoding.ASCII.GetString(b,name,Array.IndexOf(b,(byte)0,name)-name),(int)U(at));}
        uint R(int at)=>U(data+at);Vector3 V(int at)=>new(BitConverter.ToSingle(b,data+at),BitConverter.ToSingle(b,data+at+4),BitConverter.ToSingle(b,data+at+8));
        int system=classes["hknpPhysicsSystemData"],body=fix[system+64],tree=classes["hknpCompressedMeshShapeData"]+16;
        Check(R(system+72)==1&&R(body+20)==0&&R(body+24)==1&&b[data+body+16]==255,"Native fall-sensor body was converted to ordinary solid physics");
        Check(V(body+48)==Vector3.Zero&&V(body+64)==Vector3.Zero&&BitConverter.ToSingle(b,data+body+76)==1,"Native body reapplied source transforms");
        int nameAt=data+fix[body+32];Check(Encoding.ASCII.GetString(b,nameAt,Array.IndexOf(b,(byte)0,nameAt)-nameAt)==bodyName,"Havok body name and native archive model differ");
        var result=new List<(MapCollision.Triangle,float)>();var lo=V(tree+16);var hi=V(tree+32);var sharedScale=(hi-lo)/new Vector3(2097151,2097151,4194303);
        for(int s=0;s<R(tree+72);s++) {
            int at=fix[tree+64]+s*96,first=(int)R(at+72),count=(int)(R(at+80)&255),primitive=(int)(R(at+80)>>8),packedCount=(int)(R(at+76)&255),sharedFirst=(int)(R(at+76)>>8);var offset=V(at+48);var scale=V(at+60);
            Vector3 Point(int i) {
                if(i<packedCount){uint word=R(fix[tree+112]+(first+i)*4);return new Vector3(word&2047,(word>>11)&2047,word>>22)*scale+offset;}
                ushort index=BitConverter.ToUInt16(b,data+fix[tree+96]+(sharedFirst+i-packedCount)*2);ulong shared=BitConverter.ToUInt64(b,data+fix[tree+128]+index*8);return new Vector3(shared&2097151,(shared>>21)&2097151,shared>>42)*sharedScale+lo;
            }
            float tolerance=Vector3.Max(scale,sharedScale).Length()*.5f;
            for(int i=0;i<count;i++){int p=data+fix[tree+80]+(primitive+i)*4;Check(b[p+2]==b[p+3],"Authored triangle became an invented quad");result.Add((new(Point(b[p]),Point(b[p+1]),Point(b[p+2])),tolerance));}
        }
        return new(result);
    }
}
