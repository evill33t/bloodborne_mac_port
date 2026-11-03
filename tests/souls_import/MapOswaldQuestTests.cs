// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
using Spline=StudioCore.Utilities.SplineCompressedAnimation;

static class MapOswaldQuestTests {
    static void Check(bool b,string why){if(!b)throw new Exception(why);}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static string Hash(byte[] b)=>Convert.ToHexStringLower(SHA256.HashData(b));
    static int F(int source)=>MapNpcQuestState.Flag(source);
    static int Constant(byte[] b)=>b.Length==2?b[0]-64:BitConverter.ToInt32(b,1);
    static double Evaluate(byte[] code,IReadOnlyDictionary<int,bool> flags,bool action=true,bool ended=false,bool dead=false,double distance=1) {
        var stack=new List<double>();double Pop(){double n=stack[^1];stack.RemoveAt(stack.Count-1);return n;}
        for(int i=0;i<code.Length;i++){byte op=code[i];if(op<=0x7f)stack.Add(op-64);else if(op==0x82){stack.Add(BitConverter.ToInt32(code,i+1));i+=4;}
            else if(op is >=0x84 and <=0x8a){int count=op-0x84;var args=new double[count];for(int j=count-1;j>=0;j--)args[j]=Pop();int function=(int)Pop();stack.Add(function switch{1=>distance,3=>dead?1:0,15=>flags.GetValueOrDefault((int)args[0])?1:0,19=>0,56=>action?1:0,57=>ended?1:0,_=>throw new Exception("Unexpected native function "+function)});}
            else if(op is 0x91 or 0x94 or 0x95 or 0x98 or 0x99){double b=Pop(),a=Pop();stack.Add(op switch{0x91=>a<=b?1:0,0x94=>a>b?1:0,0x95=>a==b?1:0,0x98=>a!=0&&b!=0?1:0,_=>a!=0||b!=0?1:0});}
            else if(op==0xa6)Check(stack.Count>0,"Invalid continuation");else if(op==0xa1){Check(i==code.Length-1&&stack.Count==1,"Invalid ESD result");return Pop();}else throw new Exception("Unexpected opcode "+op);
        }throw new Exception("Missing expression end");
    }
    sealed class Machine(IEnumerable<EMEVD.Instruction> input,Dictionary<int,bool> flags,Dictionary<int,bool> actors,Dictionary<int,bool> dead,Func<(int Area,int Block)> map) {
        readonly EMEVD.Instruction[] code=input.ToArray();readonly Dictionary<sbyte,List<Func<bool>>> groups=[];int pc;
        bool On(int flag)=>flags.GetValueOrDefault(flag);
        bool Group(sbyte group)=>groups.TryGetValue(group,out var members)&&(group<0?members.Any(m=>m()):members.All(m=>m()));
        internal bool Run(){for(int steps=0;pc<code.Length;steps++){Check(steps<1000,"Unbounded generated event");var i=code[pc];var b=i.ArgData;
            switch((i.Bank,i.ID)) {
                case (2004,1):Check(b[4]==0,"Native NPC AI enabled");break;
                case (2004,5):actors[BitConverter.ToInt32(b)]=b[4]!=0;break;
                case (1003,1):if(On(BitConverter.ToInt32(b,4))==(b[1]!=0))pc+=b[0];break;
                case (1003,2):if(On(BitConverter.ToInt32(b,4))==(b[1]!=0)){pc=code.Length;return true;}break;
                case (3,0):{int flag=BitConverter.ToInt32(b,4);bool value=b[1]!=0;sbyte group=unchecked((sbyte)b[0]);if(group==0){if(On(flag)!=value)return false;break;}if(!groups.TryGetValue(group,out var list))groups.Add(group,list=[]);list.Add(()=>On(flag)==value);break;}
                case (3,8):{sbyte group=unchecked((sbyte)b[0]);Check(group==1&&b[1]==1,"Unexpected map condition");if(!groups.TryGetValue(group,out var list))groups.Add(group,list=[]);byte area=b[2],block=b[3];list.Add(()=>map()==(area,block));break;}
                case (0,0):{sbyte main=unchecked((sbyte)b[0]);bool value=b[1]!=0;sbyte group=unchecked((sbyte)b[2]);if(main==0){if(Group(group)!=value)return false;break;}if(!groups.TryGetValue(main,out var list))groups.Add(main,list=[]);list.Add(()=>Group(group)==value);break;}
                case (4,0):{sbyte group=unchecked((sbyte)b[0]);int actor=BitConverter.ToInt32(b,4);bool value=b[8]!=0;Check(group==1,"Unexpected death condition");if(!groups.TryGetValue(group,out var list))groups.Add(group,list=[]);list.Add(()=>dead.GetValueOrDefault(actor)==value);break;}
                case (2003,2):flags[BitConverter.ToInt32(b)]=b[4]!=0;break;
                case (1000,4):Check(b[0]==1,"Unexpected event restart");pc=0;groups.Clear();return false;
                default:throw new Exception("Unexpected generated instruction "+i.Bank+"["+i.ID+"]");
            }pc++;
        }return true;}
    }
    static float Error(Matrix4x4 a,Matrix4x4 b)=>new[]{Vector3.Zero,Vector3.UnitX,Vector3.UnitY,Vector3.UnitZ}.Max(v=>Vector3.Distance(Vector3.Transform(v,a),Vector3.Transform(v,b)));
    static Matrix4x4 Sample(Spline.TransformTrack t,float frame) {
        Vector3 Vector(bool scale){var types=scale?t.Mask.ScaleTypes:t.Mask.PositionTypes;var curve=scale?t.SplineScale:t.SplinePosition;var result=scale?Vector3.One:Vector3.Zero;var stat=scale?t.StaticScale:t.StaticPosition;for(int i=0;i<3;i++){if(curve!=null)result[i]=(i==0?curve.GetValueX(frame):i==1?curve.GetValueY(frame):curve.GetValueZ(frame))??result[i];else if(types.Contains((Spline.FlagOffset)(1<<i)))result[i]=stat[i];}return result;}
        var q=t.HasSplineRotation?t.SplineRotation.GetValue(frame):t.HasStaticRotation?t.StaticRotation:Quaternion.Identity;return Matrix4x4.CreateScale(Vector(true))*Matrix4x4.CreateFromQuaternion(Quaternion.Normalize(q))*Matrix4x4.CreateTranslation(Vector(false));
    }
    static void Main(string[] args) {
        string sourceRoot=args[0],nativeRoot=args[1],definitions=args[2],output=args[3];Directory.CreateDirectory(output);var hashes=new Dictionary<string,string>();var seed="out/map-editor/griggs-quest-tests/profile/dvdroot_ps4";
        byte[] Read(string root,string path){string full=Path.Combine(root,path);var bytes=File.ReadAllBytes(full);hashes.TryAdd(full,Hash(bytes));return bytes;}
        byte[]? Source(string path)=>File.Exists(Path.Combine(sourceRoot,path))?Read(sourceRoot,path):null;
        byte[] Native(string path)=>Read(nativeRoot,path);
        byte[]? Composed(string path)=>File.Exists(Path.Combine(seed,path))?Read(seed,path):path=="script/talk/m40_03_00_00.talkesdbnd.dcx"?Native("script/talk/m24_00_00_00.talkesdbnd.dcx"):File.Exists(Path.Combine(nativeRoot,path))?Native(path):null;
        NativeEventReservations.Verify(nativeRoot,MapOswaldQuest.FirstEvent,10);
        var sourceMap=MSB1.Read(Source("map/MapStudio/m10_01_00_00.msb")!);var original=sourceMap.Write();var target=new MSBB();var donorMap=MSBB.Read(Native("map/MapStudio/m21_00_00_00.msb.dcx"));
        var floorModel=(MSBB.Model.Collision)donorMap.Models.Collisions[0].DeepCopy();floorModel.Name="h800000";target.Models.Collisions.Add(floorModel);var floor=(MSBB.Part.Collision)donorMap.Parts.Collisions[0].DeepCopy();floor.Name="h800000_0000";floor.ModelName=floorModel.Name;target.Parts.Collisions.Add(floor);
        var from=sourceMap.Parts.Enemies.Single(p=>p.EntityID==6370);var triangles=new[]{new MapCollision.Triangle(new(-1000,-1000,-1000),new(1000,-1000,-1000),new(0,-1000,1000))};var chunks=new[]{new MapCollisionBindings.Chunk(from.CollisionName,triangles,triangles)};
        var script=new EMEVD(EMEVD.Game.Bloodborne);script.Events.Add(new(0));script.Events.Add(new(50));var sentinel=new EMEVD.Event(12416000);sentinel.Instructions.Add(new(1001,0,2f));script.Events.Add(sentinel);
        var initialized=MapNpcQuestState.Initialize(script.Write(),Source("event/common.emevd.dcx")!);var offset=new Vector3(-143.8f,20.07f,164.63f);
        var result=MapOswaldQuest.Apply(Source,Native,Composed,sourceMap,target,offset,chunks,[floor],definitions,nativeRoot,"m40_03_00_00",initialized);
        Check(original.SequenceEqual(sourceMap.Write()),"Source MSB mutated");var map=MSBB.Read(target.Write());var actor=map.Parts.Enemies.Single();Check(actor.EntityID==4036370&&actor.ModelName=="c8914"&&actor.NPCParamID==891400&&actor.ThinkParamID==-1&&actor.CharaInitID==-1&&actor.TalkID==403981&&actor.Position==from.Position+offset&&actor.Rotation==from.Rotation&&actor.CollisionName==floor.Name,"Actual Parish actor placement differs");
        Check(result.Appearance.Count==4&&result.Appearance.All(p=>p.ModelID==9270)&&result.Appearance.Select(p=>p.RequestedID).SequenceEqual(new[]{150002,151002,152002,153002}),"Black set identity differs");
        var appearance=NpcAppearance.Load(Source,definitions,6370,applyBodyShape:true);Check(appearance.Sex==1&&appearance.FaceGenID==6370&&appearance.BodyShape!=null,"Source body customization lost");
        var flver=FLVER2.Read(BND4.Read(result.Files["chr/c8914.chrbnd.dcx"]).Files.Single(f=>f.ID==200).Bytes);Check(flver.Meshes.All(m=>m.Vertices.All(v=>float.IsFinite(v.Position.LengthSquared())&&float.IsFinite(v.Normal.LengthSquared()))),"Nonfinite armor geometry");
        var nativeModel=BND4.Read(Native("chr/c8030.chrbnd.dcx")).Files.Single(f=>f.ID==200).Bytes;var skin=SkinRetarget.Bake(appearance.Model.Write(),nativeModel,appearance.Textures,textureOwner:"c8914",sourceProfile:"DS1R",preserveSourceDimensions:true);
        var sourcePose=new SourcePlayerActions(Source).Resolve(7865);Check(result.Pose.Source==7865&&result.Pose.Physical==sourcePose.MotionID&&result.Pose.Archive==sourcePose.Archive,"Arms-out source initial pose changed");
        var animation=BND4.Read(result.Files["chr/c8914.anibnd.dcx"]);var rig=NativeAnimation.Skeleton(animation.Files.Single(f=>f.ID==1000000).Bytes.ToArray());float maxError=0;int comparisons=0;
        foreach(int id in new[]{0,1000000,2000000}) {
            var raw=animation.Files.Single(f=>f.ID==1000000000+id).Bytes.ToArray();var p=new NativeAnimation.Pack(raw);int a=p.Object("hkaSplineCompressedAnimation");Check(p.Int(a+56)==sourcePose.Clip.FrameCount&&p.Float(a+16)==sourcePose.Clip.Duration,"Source pose timing differs");var data=p.Array(a+152,1,32*1024*1024);var tracks=Spline.ReadSplineCompressedAnimByteBlock(false,raw.AsSpan(p.Data+data.At,data.Count).ToArray(),rig.Names.Length,p.Int(a+60));
            foreach(int frame in new[]{0,(sourcePose.Clip.FrameCount-1)/2,sourcePose.Clip.FrameCount-1}) {
                var actual=NativeAnimation.Worlds(tracks[0].Select(t=>Sample(t,frame)).ToArray(),rig.Parents);var expected=NativeAnimation.Worlds(sourcePose.Clip.Sample(frame*sourcePose.Clip.FrameDuration),sourcePose.Clip.Bones.Select(b=>b.Parent).ToArray());
                foreach(var bone in skin.Bones.Where(b=>b.Direct)){int si=sourcePose.Clip.Bones.ToList().FindIndex(b=>b.Name==bone.Source),ti=Array.IndexOf(rig.Names,bone.Target);if(si<0||ti<0)continue;var value=expected[si];value.M41/=actor.Scale.X;value.M42/=actor.Scale.X;value.M43/=actor.Scale.X;float error=Error(actual[ti],value);maxError=Math.Max(maxError,error);Check(error<.007f,"Source gesture differs at "+id+"/"+frame+"/"+bone.Target);comparisons++;}
            }
        }
        Check(comparisons>100&&TAE.Read(animation.Files.Single(f=>f.ID==3000000).Bytes).Animations.All(a=>a.Events.Count==0),"Gesture comparison vacuous or donor effects retained");
        var beforeGame=BND4.Read(Composed("param/gameparam/gameparam.parambnd.dcx")!);var afterGame=BND4.Read(result.Files["param/gameparam/gameparam.parambnd.dcx"]);
        foreach(var member in beforeGame.Files){var next=afterGame.Files.Single(f=>f.ID==member.ID);if(Leaf(member.Name) is not("NpcParam.param" or "TalkParam.param"))Check(member.Bytes.Span.SequenceEqual(next.Bytes.Span),"Unrelated parameter changed");else {bool npc=Leaf(member.Name)=="NpcParam.param";var old=npc?MapCharacters.NativeRows(member.Bytes,false):ImportParam.Read(member.Bytes,"TALK_PARAM_ST",68,3);var now=npc?MapCharacters.NativeRows(next.Bytes,false):ImportParam.Read(next.Bytes,"TALK_PARAM_ST",68,3);Check(now.Rows.Count>=old.Rows.Count,"Prior table truncated");for(int j=0;j<old.Rows.Count;j++)Check(old.Rows[j].ID==now.Rows[j].ID&&old.Rows[j].Name==now.Rows[j].Name&&ImportParam.Payload(old.Rows[j]).SequenceEqual(ImportParam.Payload(now.Rows[j])),"Prior native/staged parameter payload/name/order changed");}}
        var talk=BND4.Read(result.Files["script/talk/m40_03_00_00.talkesdbnd.dcx"]);foreach(var f in BND4.Read(Composed("script/talk/m40_03_00_00.talkesdbnd.dcx")!).Files)Check(f.Bytes.Span.SequenceEqual(talk.Files.Single(n=>n.ID==f.ID).Bytes.Span),"Existing ESD changed");
        foreach(var(path,bytes)in result.Files.Where(p=>p.Key.StartsWith("msg/",StringComparison.Ordinal))){var before=BND4.Read(Composed(path)!);var after=BND4.Read(bytes);foreach(var member in before.Files){var next=after.Files.Single(f=>f.ID==member.ID);if(member.ID!=1)Check(member.Bytes.Span.SequenceEqual(next.Bytes.Span),"Unrelated localized FMG changed");else {var a=FMG.Read(member.Bytes);var b=FMG.Read(next.Bytes);Check(a.Entries.All(e=>b.Entries.Single(n=>n.ID==e.ID).Text==e.Text),"Prior native/staged subtitle changed");}}}
        var esd=ESD.Read(talk.Files.Single(f=>Leaf(f.Name)=="t403981.esd").Bytes);var states=esd.StateGroups[1];var flags=new Dictionary<int,bool>{{F(1700),true}};
        int? Choose(int state,bool action=true,bool ended=false,bool dead=false,double distance=1)=>states[state].Conditions.Where(c=>Evaluate(c.Evaluator,flags,action,ended,dead,distance)!=0).Select(c=>(int?)c.TargetState).FirstOrDefault();
        void Enter(int state){foreach(var c in states[state].EntryCommands.Where(c=>c.CommandID==11))flags[Constant(c.Arguments[0])]=Constant(c.Arguments[1])!=0;}
        Check(Choose(1)==null,"NPC can talk before bell arrival");flags[F(11010581)]=true;Check(Choose(1)==2&&Choose(2)==20&&Choose(20)==null&&Choose(20,ended:true)==21,"Introduction handshake differs");Check(!flags.GetValueOrDefault(F(71800057)),"Introduction complete before speech ended");Enter(21);Check(flags[F(71800057)]&&flags[F(11016111)]&&Choose(2)==30&&Choose(30,ended:true)==31,"Source repeat route lost");Enter(31);Check(flags[F(71800058)],"Source repeat completion lost");Check(Choose(20,distance:6)==3&&Choose(20,dead:true)==3&&states[3].EntryCommands.All(c=>c.CommandID!=11),"Interrupted conversation writes completion");Check(states.Values.SelectMany(s=>s.EntryCommands).All(c=>c.CommandID is not(17 or 22 or 19)),"Unsupported pardon/shop/covenant services exposed");
        var events=EMEVD.Read(result.Script);Check(events.Events.Single(e=>e.ID==12416000).Instructions.Single().ArgData.SequenceEqual(sentinel.Instructions.Single().ArgData),"Prior event changed");var flagState=new Dictionary<int,bool>();var actors=new Dictionary<int,bool>();var deadState=new Dictionary<int,bool>();
        Machine Event(int offset)=>new(events.Events.Single(e=>e.ID==MapOswaldQuest.FirstEvent+offset).Instructions,flagState,actors,deadState,()=>(40,3));
        void Pre()=>new Machine(events.Events.Single(e=>e.ID==50).Instructions,flagState,actors,deadState,()=>(40,3)).Run();
        Pre();Check(flagState[F(1700)]&&!actors[4036370],"Fresh initializer spawned Oswald before bell");var presence=Event(0);Check(!presence.Run(),"Absent bell advanced presence");flagState[MapQuestState.Flag(3)]=true;Check(!presence.Run()&&!actors[4036370],"Gargoyles defeat incorrectly substitutes bell");
        flagState[F(11010700)]=true;Check(!presence.Run()&&actors[4036370]&&flagState[F(11010581)],"Actual upper bell did not enable actor/persist arrival");var saved=JsonSerializer.Deserialize<Dictionary<int,bool>>(JsonSerializer.Serialize(flagState))!;flagState.Clear();foreach(var f in saved)flagState[f.Key]=f.Value;Pre();Check(actors[4036370],"Reload lost bell arrival");
        deadState[4036370]=true;Check(Event(1).Run()&&flagState[F(1702)]&&!flagState[F(1700)],"Oswald death not persistent");presence.Run();Check(actors[4036370],"Death removed corpse before reload");Pre();Check(!actors[4036370],"Reload resurrected Oswald");
        foreach(var(path,expected)in hashes)Check(Hash(File.ReadAllBytes(path))==expected,"Original or seed resource changed: "+path);
        foreach(var(path,bytes)in result.Files){string final=Path.Combine(output,"dvdroot_ps4",path);Directory.CreateDirectory(Path.GetDirectoryName(final)!);File.WriteAllBytes(final,bytes);}Directory.CreateDirectory(Path.Combine(output,"dvdroot_ps4/map/MapStudio"));File.WriteAllBytes(Path.Combine(output,"dvdroot_ps4/map/MapStudio/m40_03_00_00.msb.dcx"),map.Write(DCX.Type.DCX_DFLT_10000_44_9));Directory.CreateDirectory(Path.Combine(output,"dvdroot_ps4/event"));File.WriteAllBytes(Path.Combine(output,"dvdroot_ps4/event/m40_03_00_00.emevd.dcx"),result.Script);
        File.WriteAllText(Path.Combine(output,"report.json"),JsonSerializer.Serialize(new{evidence="Serialized source/native resource fixture; native gameplay not run",result.SourceName,result.Entity,result.Npc,result.Appearance,result.BodyShape,result.Pose,result.Dialogue,result.Limitations,gestureComparisons=comparisons,maxGestureError=maxError,originalAndStagedAssetsUnchanged=hashes.Count},new JsonSerializerOptions{WriteIndented=true}));
        Console.WriteLine($"PASS Oswald Parish fixture: actual Black set/arms-out pose7865, {comparisons} source/native pose comparisons (max {maxError*1000:F3}mm); real upper-bell gate distinct from Gargoyles, arrival/interruption/repeats/death/reload, source voices/locales, prior staged resources preserved; {hashes.Count} input archives unchanged. Native gameplay/services not claimed.");
    }
}
