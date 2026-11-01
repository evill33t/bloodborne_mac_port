// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
using Spline=StudioCore.Utilities.SplineCompressedAnimation;
static class HellkiteTests {
    static void Check(bool value,string why){if(!value)throw new Exception(why);}
    static void Reject(Action action,string why){try{action();}catch(InvalidDataException){return;}throw new Exception(why);}
    static string Leaf(string value)=>value.Replace('\\','/').Split('/').Last();
    sealed class VM(EMEVD.Event e,Dictionary<int,bool> flags,HashSet<int> inside,HashSet<int> dead) {
        int pc;float? until;internal float Time;internal bool Ended;
        readonly Dictionary<sbyte,List<Func<bool>>> groups=[];
        internal readonly Dictionary<int,bool> Enabled=[];internal readonly List<(int Actor,int Action)> Animations=[];internal readonly Dictionary<int,int> Warps=[];
        bool Group(sbyte id)=>groups.TryGetValue(id,out var g)&&(id<0?g.Any(f=>f()):g.All(f=>f()));
        void Condition(sbyte id,Func<bool> f){if(!groups.TryGetValue(id,out var g))groups.Add(id,g=[]);g.Add(f);}
        internal void Run() {
            if(Ended)return;
            for(int step=0;pc<e.Instructions.Count;step++) {
                Check(step<1000,"Native event failed to reach a wait");var i=e.Instructions[pc];var b=i.ArgData;int Int(int at=0)=>BitConverter.ToInt32(b,at);
                switch((i.Bank,i.ID)) {
                    case(1003,2):Check(b[2]==0,"Unmapped relative flag");if(flags.GetValueOrDefault(Int(4))==(b[1]!=0)){Ended=true;return;}break;
                    case(1003,1):if(flags.GetValueOrDefault(Int(4))==(b[1]!=0))pc+=b[0];break;
                    case(3,0):{int id=Int(4);bool on=b[1]!=0;Condition(unchecked((sbyte)b[0]),()=>flags.GetValueOrDefault(id)==on);break;}
                    case(3,2):{int id=Int(8);bool on=b[1]!=0;sbyte group=unchecked((sbyte)b[0]);if(group==0){if(inside.Contains(id)!=on)return;}else Condition(group,()=>inside.Contains(id)==on);break;}
                    case(4,0):{int actor=Int(4);Condition(unchecked((sbyte)b[0]),()=>dead.Contains(actor));break;}
                    case(0,0):if(!Group(unchecked((sbyte)b[2])))return;break;
                    case(1001,0):until??=Time+BitConverter.ToSingle(b);if(Time<until)return;until=null;break;
                    case(2003,2):flags[Int()]=b[4]!=0;break;
                    case(2003,18):Animations.Add((Int(),Int(4)));break;
                    case(2004,5):Enabled[Int()]=b[4]!=0;break;
                    case(2004,41):Warps[Int()]=Int(8);break;
                    case(2004,1):Check(b[4]==0,"Unrelated donor combat AI was enabled");break;
                    case(2004,9):case(2004,10):case(2004,15):case(2004,31):case(2004,34):case(2004,35):break;
                    default:throw new Exception("Unexpected event instruction "+i.Bank+"/"+i.ID);
                }
                pc++;
            }
            Ended=true;
        }
    }
    static Matrix4x4 Sample(Spline.TransformTrack t,float frame) {
        Vector3 p=t.StaticPosition,s=t.StaticScale;if(t.HasSplinePosition)p=new(t.SplinePosition.GetValueX(frame)??p.X,t.SplinePosition.GetValueY(frame)??p.Y,t.SplinePosition.GetValueZ(frame)??p.Z);
        if(t.HasSplineScale)s=new(t.SplineScale.GetValueX(frame)??s.X,t.SplineScale.GetValueY(frame)??s.Y,t.SplineScale.GetValueZ(frame)??s.Z);
        return Matrix4x4.CreateScale(s)*Matrix4x4.CreateFromQuaternion(Quaternion.Normalize(t.HasSplineRotation?t.SplineRotation.GetValue(frame):t.StaticRotation))*Matrix4x4.CreateTranslation(p);
    }
    static float Error(Matrix4x4 a,Matrix4x4 b) {
        float[] x=[a.M11,a.M12,a.M13,a.M21,a.M22,a.M23,a.M31,a.M32,a.M33,a.M41,a.M42,a.M43],y=[b.M11,b.M12,b.M13,b.M21,b.M22,b.M23,b.M31,b.M32,b.M33,b.M41,b.M42,b.M43];return x.Zip(y,(x,y)=>Math.Abs(x-y)).Max();
    }
    public static void Main(string[] args) {
        string sourceRoot=args[0],native=args[1],defs=args[2],output=args[3];var hashes=new Dictionary<string,string>();
        byte[] Read(string path){var b=File.ReadAllBytes(path);hashes.TryAdd(path,Convert.ToHexStringLower(SHA256.HashData(b)));return b;}
        byte[]? Source(string path)=>File.Exists(Path.Combine(sourceRoot,path))?Read(Path.Combine(sourceRoot,path)):null;
        byte[]? Native(string path)=>File.Exists(Path.Combine(native,path))?Read(Path.Combine(native,path)):null;
        var plan=HellkiteSource.Load(Source);
        var changed=EMEVD.Read(Source("event/m10_01_00_00.emevd.dcx")!);changed.Events.Single(e=>e.ID==11010790).Instructions[6].ArgData[8]^=1;
        Reject(()=>HellkiteSource.Load(p=>p=="event/m10_01_00_00.emevd.dcx"?changed.Write():Source(p)),"Changed source trigger instructions were accepted");
        var map=MSBB.Read(Native("map/MapStudio/m24_00_00_00.msb.dcx")!);MapReplacement.ParkActors(map);
        var offset=new Vector3(-143.8f,20.07f,164.63f);var slot=map.Parts.Collisions.First();
        var floors=new[]{plan.Bridge,plan.Flyby}.Select(a=>a.Position+offset).Select(p=>new MapCollision.Triangle(p+new Vector3(-5,0,-5),p+new Vector3(5,0,-5),p+new Vector3(0,0,5))).ToArray();
        var script=new EMEVD(EMEVD.Game.Bloodborne);script.Events.Add(new(0));script.Events.Add(new(50));var sentinel=new EMEVD.Event(12415900);sentinel.Instructions.Add(new(1001,0,9f));script.Events.Add(sentinel);
        var result=MapHellkite.Apply(Source,Native,map,offset,[new("fixture",floors,[])],[slot],Native("param/gameparam/gameparam.parambnd.dcx")!,script.Write(),native,defs);
        Check(result.Spawns.Count==2&&result.Spawns.Select(s=>s.EntityID).SequenceEqual(new[]{4039970,4039971})&&result.Spawns.All(s=>s.TargetModel=="c8960"&&s.ProportionScale==1&&s.DirectWeightShare>.999999),"Source actor identity/proportions changed");
        var nativeNpc=BND4.Read(Native("param/gameparam/gameparam.parambnd.dcx")!);var game=BND4.Read(result.Files["param/gameparam/gameparam.parambnd.dcx"]);
        var before=MapCharacters.NativeRows(nativeNpc.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes,false);var after=MapCharacters.NativeRows(game.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes,false);
        Check(after.Rows.Count==before.Rows.Count+1&&before.Rows.All(r=>MapCharacters.Payload(r).SequenceEqual(MapCharacters.Payload(after.Rows.Single(a=>a.ID==r.ID)))),"Native NPC row changed");
        var npc=MapCharacters.Payload(after.Rows.Single(r=>r.ID==896000));Check(BitConverter.ToInt32(npc,32)==1840&&BitConverter.ToInt32(npc,40)==10000,"Source base HP/reward lost");
        foreach(var f in nativeNpc.Files.Where(f=>Leaf(f.Name)!="NpcParam.param"))Check(game.Files.Single(a=>a.ID==f.ID).Bytes.Span.SequenceEqual(f.Bytes.Span),"Unrelated native parameter changed");
        var animation=BND4.Read(result.Files["chr/c8960.anibnd.dcx"]);var rig=NativeAnimation.Skeleton(animation.Files.Single(f=>f.ID==1000000).Bytes.ToArray(),allowMirroredScale:true);Check(rig.Names.Length==242,"Native skin limit exceeded");Matrix4x4.Invert(rig.Local[0],out var inverse);
        int poses=0;float maximum=0;
        Check(result.Details.Clips.Select(c=>c.Action).Order().SequenceEqual(new[]{0,7005,7006,7012,1000000,1007005,1007006,1007012}),"Both native cinematic banks must be adapted");
        var timeActs=TAE.Read(animation.Files.Single(f=>f.ID==3000000).Bytes);
        foreach(var clip in result.Details.Clips)Check(((TAE.Animation.AnimMiniHeader.Standard)timeActs.Animations.Single(a=>a.ID==clip.Action).MiniHeader).IsLoopByDefault==(clip.SourceAction==7006),"Perch must loop in both native behavior banks");
        Check(timeActs.Animations.Where(a=>!result.Details.Clips.Any(c=>c.Action==a.ID)).All(a=>a.Events.Count==0&&a.EventGroups.Count==0),"Unrelated donor action effects survived in the cinematic wrapper");
        foreach(var binding in result.Details.Clips) {
            var bytes=animation.Files.Single(f=>f.ID==1000000000+binding.Action).Bytes.ToArray();var pack=new NativeAnimation.Pack(bytes);int a=pack.Object("hkaSplineCompressedAnimation"),frame=pack.Object("hkaDefaultAnimatedReferenceFrame");
            Check(Math.Abs(pack.Float(a+16)-binding.Duration)<.00001f&&pack.Int(a+20)==242&&pack.Int(a+56)==binding.Frames,"Native duration/dimensions changed");var data=pack.Array(a+152,1,64*1024*1024);var root=pack.Array(frame+56,16,1024);
            var decoded=Spline.ReadSplineCompressedAnimByteBlock(false,bytes.AsSpan(pack.Data+data.At,data.Count).ToArray(),242,1)[0];var source=SourceAnimation.Load(Source,"c3430",binding.SourceAction);
            foreach(int f in new[]{0,binding.Frames/2,binding.Frames-1}) {
                float fraction=(float)f/(binding.Frames-1);var expected=source.Sample(fraction*source.Duration);Check(Matrix4x4.Invert(Sample(decoded[0],f),out var nativeInverse),"Singular native root");expected[0]*=nativeInverse;
                for(int bone=0;bone<132;bone++){float error=Error(expected[bone],Sample(decoded[110+bone],f));maximum=Math.Max(maximum,error);Check(error<.005f,"Independent source pose differs: "+binding.Action+"/"+bone+"="+error);}
                var expectedRoot=binding.SourceAction==7006?Vector4.Zero:plan.Motions[binding.SourceAction].At(fraction);int rootIndex=(int)MathF.Round(fraction*(root.Count-1));expectedRoot=binding.SourceAction==7006?Vector4.Zero:plan.Motions[binding.SourceAction].At((float)rootIndex/(root.Count-1));int at=pack.Data+root.At+rootIndex*16;var actual=new Vector4(BitConverter.ToSingle(bytes,at),BitConverter.ToSingle(bytes,at+4),BitConverter.ToSingle(bytes,at+8),BitConverter.ToSingle(bytes,at+12));Check(Vector4.Distance(expectedRoot,actual)<.0001f,"Source flight trajectory differs");poses++;
            }
        }
        var events=EMEVD.Read(result.Script);Check(events.Events.Single(e=>e.ID==sentinel.ID).Instructions.Single().ArgData.SequenceEqual(sentinel.Instructions.Single().ArgData),"Existing event changed");
        var flags=new Dictionary<int,bool>();var inside=new HashSet<int>();var dead=new HashSet<int>();var d=result.Details;
        VM Machine(int id)=>new(events.Events.Single(e=>e.ID==id),flags,inside,dead);
        Machine(50).Run();var fly=Machine(MapHellkite.FirstEvent);var bridge=Machine(MapHellkite.FirstEvent+1);fly.Run();bridge.Run();Check(!fly.Animations.Any()&&!bridge.Animations.Any(),"Dragon starts without source trigger");
        inside.Add(d.FlybyTrigger);fly.Run();Check(fly.Animations.SequenceEqual(new[]{(4039971,7012)})&&flags[MapHellkite.FlybyRunning],"Burg flyby did not start");
        inside.Add(d.BridgeTrigger);bridge.Run();Check(!bridge.Animations.Any(),"Bridge arrival overlaps first flyby");fly.Time=10;fly.Run();Check(fly.Enabled[4039971]==false&&!flags[MapHellkite.FlybyRunning],"Completed flyby remained visible");
        inside.Add(d.BridgeExclusion);bridge.Run();Check(!bridge.Animations.Any(),"Source far-end exclusion was ignored");inside.Remove(d.BridgeExclusion);
        bridge.Run();Check(bridge.Animations.Single()==(4039970,7005),"Bridge approach did not start");bridge.Time=14;bridge.Run();Check(bridge.Animations.Last()==(4039970,7006)&&bridge.Warps[4039970]==d.Perch&&flags[MapHellkite.BridgeReady],"Bridge did not restore source perch");
        Machine(50).Run();var repeat=Machine(MapHellkite.FirstEvent+1);repeat.Run();Check(repeat.Animations.SequenceEqual(new[]{(4039970,7006)}),"Seen bridge approach replayed after reload");var noFly=Machine(MapHellkite.FirstEvent);noFly.Run();Check(!noFly.Animations.Any(),"Seen Burg flyby replayed");
        var death=Machine(MapHellkite.FirstEvent+2);death.Run();Check(!flags.GetValueOrDefault(MapHellkite.Defeated),"Living dragon defeated");dead.Add(4039970);death.Run();Check(flags[MapHellkite.Defeated],"Bridge defeat was not persisted");Machine(50).Run();var absent=Machine(MapHellkite.FirstEvent+1);absent.Run();Check(!absent.Animations.Any(),"Defeated dragon reappeared");
        foreach(var (path,sha) in hashes)Check(Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(path)))==sha,"Original asset changed: "+path);
        Directory.CreateDirectory(output);foreach(var pair in result.Files.Where(f=>f.Key.StartsWith("chr/",StringComparison.Ordinal))){string path=Path.Combine(output,pair.Key);Directory.CreateDirectory(Path.GetDirectoryName(path)!);File.WriteAllBytes(path,pair.Value);}
        File.WriteAllText(Path.Combine(output,"validation.json"),JsonSerializer.Serialize(new{sourcePoses=poses,maximumPoseError=maximum,clips=result.Details.Clips,events=result.Details.SourceEvents,originalHashes=hashes},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));
        Console.WriteLine($"PASS: two Hellkite actors,242 bones,{poses} independently decoded poses/flight samples, source triggers, first-visit/reload/defeat state machines, {hashes.Count} original hashes unchanged");
    }
}
