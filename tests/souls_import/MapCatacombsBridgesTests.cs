// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Text.Json;
using System.Security.Cryptography;
using StudioCore.Utilities;

static class MapCatacombsBridgesTests {
    static void Check(bool b,string why){if(!b)throw new Exception(why);}
    sealed class Machine(IEnumerable<EMEVD.Instruction> input,Dictionary<int,bool> flags,HashSet<int> regions,HashSet<int> actions) {
        readonly EMEVD.Instruction[] code=input.ToArray();readonly Dictionary<sbyte,List<Func<bool>>> groups=[];readonly Dictionary<sbyte,bool> finished=[];int pc;
        internal readonly List<(int Entity,int Animation,bool End)> Animations=[];internal readonly Dictionary<int,bool> Enabled=[];internal readonly List<float> Waits=[];
        bool On(int n)=>flags.GetValueOrDefault(n);
        bool Group(sbyte n)=>groups.TryGetValue(n,out var list)&&(n<0?list.Any(f=>f()):list.All(f=>f()));
        void Condition(sbyte n,Func<bool> f){if(!groups.TryGetValue(n,out var list))groups.Add(n,list=[]);list.Add(f);}
        internal bool Run(){for(int steps=0;pc<code.Length;steps++){Check(steps<1000,"Unbounded mechanism event");var i=code[pc];var b=i.ArgData;int N(int at=0)=>BitConverter.ToInt32(b,at);
            switch((i.Bank,i.ID)) {
                case (2005,6):Enabled[N(4)]=b[8]!=0;break;
                case (2005,7):Animations.Add((N(),N(4),true));break;
                case (2003,18):Animations.Add((N(),N(4),false));break;
                case (2003,2):flags[N()]=b[4]!=0;break;
                case (1003,1):if(On(N(4))==(b[1]!=0))pc+=b[0];break;
                case (1003,2):if(On(N(4))==(b[1]!=0)){pc=code.Length;return true;}break;
                case (1000,3):pc+=b[0];break;
                case (1000,7):if(finished.GetValueOrDefault(unchecked((sbyte)b[2]))==(b[1]!=0))pc+=b[0];break;
                case (1000,4):Check(b[0]==1,"Unexpected mechanism ending");pc=0;groups.Clear();finished.Clear();return true;
                case (1001,0):Waits.Add(BitConverter.ToSingle(b));break;
                case (3,0): {sbyte group=unchecked((sbyte)b[0]);int flag=N(4);bool state=b[1]!=0;if(group==0){if(On(flag)!=state)return false;}else Condition(group,()=>On(flag)==state);break;}
                case (3,2): {sbyte group=unchecked((sbyte)b[0]);int region=N(8);bool state=b[1]!=0;Condition(group,()=>regions.Contains(region)==state);break;}
                case (5,2):if(!actions.Remove(N(4)))return false;break;
                case (0,0): {sbyte target=unchecked((sbyte)b[0]),source=unchecked((sbyte)b[2]);bool state=b[1]!=0;if(target!=0){Condition(target,()=>Group(source)==state);break;}if(Group(source)!=state)return false;foreach(var n in groups.Keys)finished[n]=Group(n);break;}
                default:throw new Exception("Unexpected generated native instruction "+i.Bank+"["+i.ID+"]");
            }pc++;
        }return true;}
    }
    static Matrix4x4 Track(SplineCompressedAnimation.TransformTrack t,float f) {
        var p=t.HasSplinePosition?new(t.SplinePosition.GetValueX(f)??t.StaticPosition.X,t.SplinePosition.GetValueY(f)??t.StaticPosition.Y,t.SplinePosition.GetValueZ(f)??t.StaticPosition.Z):t.StaticPosition;
        var s=t.HasSplineScale?new(t.SplineScale.GetValueX(f)??t.StaticScale.X,t.SplineScale.GetValueY(f)??t.StaticScale.Y,t.SplineScale.GetValueZ(f)??t.StaticScale.Z):t.StaticScale;
        return Matrix4x4.CreateScale(s)*Matrix4x4.CreateFromQuaternion(t.HasSplineRotation?t.SplineRotation.GetValue(f):t.StaticRotation)*Matrix4x4.CreateTranslation(p);
    }
    static void Main(string[] args) {
        var hashes=new Dictionary<string,string>();string Hash(byte[] raw)=>Convert.ToHexStringLower(SHA256.HashData(raw));
        byte[] Read(string root,string path){string file=Path.Combine(root,path);var raw=File.ReadAllBytes(file);hashes[file]=Hash(raw);return raw;}
        MapCatacombsBridges.VerifyReservations(args[1]);
        var donor=BND4.Read(Read(args[1],"obj/o241013.objbnd.dcx"));var original=donor.Files.Single(f=>f.ID==300).Bytes.ToArray();
        var bridge=BND3.Read(Read(args[0],"obj/o3001.objbnd.dcx"));var hull=CatacombsBridgePhysics.Source(bridge.Files.Single(f=>f.ID==300).Bytes.ToArray());
        var physics=CatacombsBridgePhysics.Convert(original,hull);var p=new NativeAnimation.Pack(physics);int shape=p.Object("hknpConvexPolytopeShape");
        int Relative(int field,int count){Check(BitConverter.ToUInt16(physics,p.Data+shape+field)==count,"Hull array count changed");return shape+field+BitConverter.ToUInt16(physics,p.Data+shape+field+2);}
        int va=Relative(40,24),pa=Relative(44,24),fa=Relative(48,22),ix=Relative(52,88);float shapeError=0;
        for(int i=0;i<24;i++) {
            Vector3 actual=new(p.Float(va+i*16),p.Float(va+i*16+4),p.Float(va+i*16+8));shapeError=Math.Max(shapeError,Vector3.Distance(actual,hull.Vertices[i]));
            Check(BitConverter.ToUInt32(physics,p.Data+va+i*16+12)==(0x3f000000u|(uint)i),"Native support vertex ID lost");
            var plane=new Vector4(p.Float(pa+i*16),p.Float(pa+i*16+4),p.Float(pa+i*16+8),p.Float(pa+i*16+12));Check(plane==hull.Planes[i],"Source bridge plane changed");
        }
        Check(shapeError==0,"Source bridge hull changed");int cursor=0;
        for(int i=0;i<22;i++){Check(BitConverter.ToUInt16(physics,p.Data+fa+i*4)==cursor&&physics[p.Data+fa+i*4+2]==hull.Faces[i].Length&&physics[p.Data+fa+i*4+3]==CatacombsBridgePhysics.FaceAngle(hull,i),"Native face cache/index header differs");Check(physics.AsSpan(p.Data+ix+cursor,hull.Faces[i].Length).SequenceEqual(hull.Faces[i]),"Source face connectivity changed");cursor+=hull.Faces[i].Length;}
        var ptr=CatacombsBridgePhysics.Pointers(physics);Check(ptr[560]==shape&&ptr[672]==shape&&ptr[shape+32]==1104,"Physics body/property shared references changed");
        int faceCacheChecks=0;
        foreach(string precedent in new[]{"o294642","o293802"}) {
            var raw=BND4.Read(Read(args[1],"obj/"+precedent+".objbnd.dcx")).Files.Single(f=>f.ID==300).Bytes.ToArray();var pp=new NativeAnimation.Pack(raw);int sh=pp.Object("hknpConvexPolytopeShape");
            int count=BitConverter.ToUInt16(raw,pp.Data+sh+48),planes=sh+44+BitConverter.ToUInt16(raw,pp.Data+sh+46),faces=sh+48+BitConverter.ToUInt16(raw,pp.Data+sh+50),indices=sh+52+BitConverter.ToUInt16(raw,pp.Data+sh+54);
            var normals=Enumerable.Range(0,count).Select(i=>new Vector4(pp.Float(planes+i*16),pp.Float(planes+i*16+4),pp.Float(planes+i*16+8),pp.Float(planes+i*16+12))).ToArray();
            var topology=Enumerable.Range(0,count).Select(i=>raw.AsSpan(pp.Data+indices+BitConverter.ToUInt16(raw,pp.Data+faces+i*4),raw[pp.Data+faces+i*4+2]).ToArray()).ToArray();
            var hullPrecedent=new CatacombsBridgePhysics.Hull([],normals,topology,0);for(int i=0;i<count;i++){Check(CatacombsBridgePhysics.FaceAngle(hullPrecedent,i)==raw[pp.Data+faces+i*4+3],"Native non-box minHalfAngle quantization precedent differs");faceCacheChecks++;}
        }
        // All old scene/body/motion/material bytes remain byte-identical. Only
        // fixup targets and the new convex shape are different.
        Check(physics.AsSpan(p.Data,720).SequenceEqual(original.AsSpan(new NativeAnimation.Pack(original).Data,720)),"Original native scene/controller data changed");
        float poseError=0;int poses=0;
        foreach(string model in new[]{"o3001","o3011"}) {
            var source=model=="o3001"?bridge:BND3.Read(Read(args[0],"obj/"+model+".objbnd.dcx"));var sourceBank=BND3.Read(source.Files.Single(f=>f.ID==400).Bytes);
            var converted=CatacombsBridgeAnimations.Convert(model,source.Files.Single(f=>f.ID==400).Bytes.ToArray(),donor.Files.Single(f=>f.ID==400).Bytes.ToArray());
            var anim=BND4.Read(converted.Bundle);var rig=NativeAnimation.Skeleton(anim.Files.Single(f=>f.ID==1000000).Bytes.ToArray());Check(Matrix4x4.Invert(converted.SourceLeafReference,out var inverse),"Source reference singular");
            foreach(int id in new[]{0,1,2,3}) {
                var raw=anim.Files.Single(f=>f.ID==1000000000+id).Bytes.ToArray();var pack=new NativeAnimation.Pack(raw);int at=pack.Object("hkaSplineCompressedAnimation");var bytes=pack.Array(at+152,1,1000000);int frameCount=pack.Int(at+56);
                var tracks=SplineCompressedAnimation.ReadSplineCompressedAnimByteBlock(false,raw.AsSpan(pack.Data+bytes.At,bytes.Count).ToArray(),2,1).Single();
                var clip=SourceAnimation.Decode(sourceBank.Files.Single(f=>f.ID==1000000).Bytes.ToArray(),sourceBank.Files.Single(f=>f.ID==(id==3?3:1)).Bytes.ToArray());
                foreach(int frame in new[]{0,(frameCount-1)/2,frameCount-1}) {
                    float time=id==0?0:id==2?clip.Duration:frame/30f;var expected=rig.Local[1]*inverse*clip.Sample(time)[1];var actual=Track(tracks[1],frame);
                    foreach(var v in new[]{Vector3.Zero,new Vector3(10,1,2)})poseError=Math.Max(poseError,Vector3.Distance(Vector3.Transform(v,expected),Vector3.Transform(v,actual)));poses++;
                }
            }
        }
        Check(poseError<.005f,"Source mechanism playback pose differs");
        byte[]? Source(string path)=>File.Exists(Path.Combine(args[0],path))?Read(args[0],path):null;
        byte[]? Native(string path)=>File.Exists(Path.Combine(args[1],path))?Read(args[1],path):null;
        var sourceMap=MSB1.Read(Source("map/MapStudio/m13_00_00_00.msb")!);var sourceBefore=sourceMap.Write();
        var nativeMap=MSBB.Read(Native("map/MapStudio/m24_02_00_00.msb.dcx")!);var target=new MSBB();var floorModel=(MSBB.Model.Collision)nativeMap.Models.Collisions[0].DeepCopy();floorModel.Name="h800000";target.Models.Collisions.Add(floorModel);
        var floor=(MSBB.Part.Collision)nativeMap.Parts.Collisions[0].DeepCopy();floor.Name="h800000_0000";floor.ModelName=floorModel.Name;target.Parts.Collisions.Add(floor);
        var triangles=new[]{new MapCollision.Triangle(new(-1000,-1000,-1000),new(1000,-1000,-1000),new(0,-1000,1000))};var chunks=new[]{new MapCollisionBindings.Chunk("fixture floor",triangles,triangles)};
        var script=new EMEVD(EMEVD.Game.Bloodborne);script.Events.Add(new(0));script.Events.Add(new(50));var sentinel=new EMEVD.Event(12416400);sentinel.Instructions.Add(new(1001,0,1f));script.Events.Add(sentinel);
        var surface=FLVER2.Read(Native("map/m24_01_00_00/m24_01_00_00_004570.flver.dcx")!);
        FLVER2.Material Material(IFlverMaterial m){var donor=surface.Materials[0];return new(m.Name,donor.MTD,0){GXIndex=donor.GXIndex,Textures=donor.Textures.Select(t=>new FLVER2.Texture(t.ParamName,t.Path,t.TilingScale,t.TilingTypeU,t.TilingTypeV,t.Unk14,t.Unk18,t.Unk1C)).ToList()};}
        var offset=new Vector3(-143.8f,20.07f,164.63f);var result=MapCatacombsBridges.Apply(Source,Native,_=>null,sourceMap,target,offset,chunks,[floor],script.Write(),"m13_00_00_00","m40_05_00_00",surface,Material);
        var reread=MSBB.Read(target.Write());Check(result.Bindings.Count==2&&reread.Parts.Objects.Count==2&&reread.Events.ObjActs.Count==2&&reread.Regions.Regions.Count==2&&result.ConsumedNames.SetEquals(new[]{"o3011_0002","o3001_0000"}),"Missing or duplicate native mechanism actors/actions/regions");
        foreach(var binding in result.Bindings){var from=sourceMap.Parts.Objects.Single(p=>p.Name==binding.SourceName);var to=reread.Parts.Objects.Single(p=>p.EntityID==binding.Entity);Check(to.Position==from.Position+offset&&to.Rotation==from.Rotation&&to.CollisionName==floor.Name,"Authored mechanism frame lost");}
        foreach(var action in reread.Events.ObjActs)Check(action.ObjActPartName=="o696100_0000"&&action.EventFlagID==0&&action.ObjActState==0,"Native lever action controller detached");
        var leverVisual=FLVER2.Read(BND4.Read(result.Files["obj/o696100.objbnd.dcx"]).Files.Single(f=>f.ID==200).Bytes);Check(leverVisual.Dummies.Select(d=>d.ReferenceID).Order().SequenceEqual(new short[]{191,192}),"Source lever action anchors lost");
        var leverPhysics=new NativeAnimation.Pack(BND4.Read(result.Files["obj/o696100.objbnd.dcx"]).Files.Single(f=>f.ID==300).Bytes.ToArray());Check(Math.Abs(leverPhysics.Float(720+64+4)-(1.1635761f+.038285017f))<.00001f,"Lever collider not in authored model reference space");
        int Flag(int source)=>MapNpcQuestState.Flag(source);var flags=new Dictionary<int,bool>{{Flag(1620),true}};var regions=new HashSet<int>();var actions=new HashSet<int>();var events=EMEVD.Read(result.Script);
        Check(events.Events.Count==script.Events.Count+6&&events.Events.Single(e=>e.ID==12416400).Instructions.Single().ArgData.SequenceEqual(sentinel.Instructions.Single().ArgData),"Original event ownership changed");
        Machine Event(int n)=>new(events.Events.Single(e=>e.ID==MapCatacombsBridges.FirstEvent+n).Instructions,flags,regions,actions);
        var controller=Event(0);Check(!controller.Run()&&controller.Enabled[696110]&&!controller.Enabled[696111]&&controller.Animations.Contains((MapCatacombsBridges.Bridge,2,true)),"Fresh bridge does not restore source vertical pose");
        var approach=Event(3);Check(!approach.Run(),"Trap fires outside source approach volume");regions.Add(MapCatacombsBridges.ApproachRegion);Check(approach.Run()&&flags[Flag(11300592)]&&flags[Flag(11305037)]&&approach.Animations.Contains((MapCatacombsBridges.Lever,1,false)),"Approach does not pull real lever");
        Check(controller.Run()&&flags[Flag(11300403)]&&controller.Animations.Contains((MapCatacombsBridges.Bridge,3,false))&&controller.Enabled[696111]&&controller.Waits.Single()==140/30f,"Bridge lowering/source timing/action handoff failed");
        var crossing=Event(4);Check(!crossing.Run(),"Trap closes before source crossing volume");regions.Add(MapCatacombsBridges.CrossRegion);Check(crossing.Run()&&flags[Flag(11300593)]&&flags[Flag(11305038)],"Source crossing trap missing");
        Check(controller.Run()&&!flags[Flag(11300403)]&&controller.Animations.Contains((MapCatacombsBridges.Bridge,1,false)),"Bridge does not physically rotate upward on trap");
        Check(Event(5).Run()&&flags[Flag(1621)]&&!flags[Flag(1620)],"Patches source trap quest transition lost");
        var reload=Event(0);Check(!reload.Run()&&reload.Animations.Contains((MapCatacombsBridges.Bridge,2,true)),"Bridge persistent reload orientation failed");
        var manual=Event(1);Check(!manual.Run(),"Manual lever fires without activation");actions.Add(MapCatacombsBridges.LowerAction);Check(manual.Run()&&flags[Flag(11305037)]&&reload.Run()&&flags[Flag(11300403)],"Manual lever cannot reuse trapped bridge");
        Check(sourceMap.Write().SequenceEqual(sourceBefore),"Source MSB mutated");
        foreach(var pair in hashes)Check(Hash(File.ReadAllBytes(pair.Key))==pair.Value,"Original mechanism archive changed");
        string output="out/map-editor/patches-bridges/fixture";Directory.CreateDirectory(output);File.WriteAllBytes(Path.Combine(output,"bridge-physics.hkx"),physics);
        foreach(var file in result.Files){string path=Path.Combine(output,"package",file.Key);Directory.CreateDirectory(Path.GetDirectoryName(path)!);File.WriteAllBytes(path,file.Value);}File.WriteAllBytes(Path.Combine(output,"map.msb.dcx"),target.Write(DCX.Type.DCX_DFLT_10000_44_9));File.WriteAllBytes(Path.Combine(output,"map.emevd.dcx"),result.Script);
        File.WriteAllText(Path.Combine(output,"validation.json"),JsonSerializer.Serialize(new{status="PASS",shapeError,poseError,poses,hullFaces=hull.Faces.Length,faceCache=hull.Faces.Select((f,i)=>CatacombsBridgePhysics.FaceAngle(hull,i)),hashes},new JsonSerializerOptions{WriteIndented=true}));
        Console.WriteLine($"PASS Catacombs bridge fixture: exact 24 vertices / 24 planes / 22 faces / 88 indices; {faceCacheChecks} shipped face caches; 24 source motion poses, max error {poseError*1000:F3}mm; native installation, trap/manual controls, persistence and quest transition. Gameplay pending.");
    }
}
