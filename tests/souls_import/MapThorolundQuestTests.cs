// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
using Spline=StudioCore.Utilities.SplineCompressedAnimation;

static class MapThorolundQuestTests {
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
                case (2004,12):Check(b[4]==1,"Unexpected immortality command");break;
                case (1000,1):if(Group(unchecked((sbyte)b[2]))==(b[1]!=0))pc+=b[0];break;
                case (2004,5):actors[BitConverter.ToInt32(b)]=b[4]!=0;break;
                case (1003,1):if(On(BitConverter.ToInt32(b,4))==(b[1]!=0))pc+=b[0];break;
                case (1003,2):if(On(BitConverter.ToInt32(b,4))==(b[1]!=0)){pc=code.Length;return true;}break;
                case (3,0):{int flag=BitConverter.ToInt32(b,4);bool value=b[1]!=0;sbyte group=unchecked((sbyte)b[0]);if(group==0){if(On(flag)!=value)return false;break;}if(!groups.TryGetValue(group,out var list))groups.Add(group,list=[]);list.Add(()=>On(flag)==value);break;}
                case (3,8):{sbyte group=unchecked((sbyte)b[0]);Check(group==1&&b[1]==1,"Unexpected map condition");if(!groups.TryGetValue(group,out var list))groups.Add(group,list=[]);byte area=b[2],block=b[3];list.Add(()=>map()==(area,block));break;}
                case (0,0):{sbyte main=unchecked((sbyte)b[0]);bool value=b[1]!=0;sbyte group=unchecked((sbyte)b[2]);if(main==0){if(Group(group)!=value)return false;break;}if(!groups.TryGetValue(main,out var list))groups.Add(main,list=[]);list.Add(()=>Group(group)==value);break;}
                case (4,0):{sbyte group=unchecked((sbyte)b[0]);int actor=BitConverter.ToInt32(b,4);bool value=b[8]!=0;Check(group==1,"Unexpected death condition");if(!groups.TryGetValue(group,out var list))groups.Add(group,list=[]);list.Add(()=>dead.GetValueOrDefault(actor)==value);break;}
                case (2003,2):flags[BitConverter.ToInt32(b)]=b[4]!=0;break;
                case (1000,3):pc+=b[0];break;
                case (1000,4):if(b[0]==0){pc=code.Length;return true;}Check(b[0]==1,"Unexpected event restart");pc=0;groups.Clear();return false;
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
        string sourceRoot=args[0],nativeRoot=args[1],definitions=args[2],output=args[3];Directory.CreateDirectory(output);var hashes=new Dictionary<string,string>();const string seed="out/map-editor/andre-quest-tests/profile/dvdroot_ps4";
        byte[] Read(string root,string path){string full=Path.Combine(root,path);var b=File.ReadAllBytes(full);hashes.TryAdd(full,Hash(b));return b;}
        byte[]? Source(string path)=>File.Exists(Path.Combine(sourceRoot,path))?Read(sourceRoot,path):null;
        byte[] Native(string path)=>Read(nativeRoot,path);
        byte[]? Composed(string path)=>File.Exists(Path.Combine(seed,path))?Read(seed,path):File.Exists(Path.Combine("out/map-editor/griggs-quest-tests/profile/dvdroot_ps4",path))?Read("out/map-editor/griggs-quest-tests/profile/dvdroot_ps4",path):path=="script/talk/m40_02_00_00.talkesdbnd.dcx"?Native("script/talk/m24_00_00_00.talkesdbnd.dcx"):File.Exists(Path.Combine(nativeRoot,path))?Native(path):null;
        NativeEventReservations.Verify(nativeRoot,MapThorolundEvents.FirstEvent,20);
        var sourceMap=MSB1.Read(Source("map/MapStudio/m10_02_00_00.msb")!);var sourceBefore=sourceMap.Write();var target=new MSBB();var nativeMap=MSBB.Read(Native("map/MapStudio/m21_00_00_00.msb.dcx"));
        var fm=(MSBB.Model.Collision)nativeMap.Models.Collisions[0].DeepCopy();fm.Name="h800000";target.Models.Collisions.Add(fm);var floor=(MSBB.Part.Collision)nativeMap.Parts.Collisions[0].DeepCopy();floor.Name="h800000_0000";floor.ModelName=fm.Name;target.Parts.Collisions.Add(floor);
        const int petrus=2419701;var prior=(MSBB.Part.Enemy)nativeMap.Parts.Enemies.First(p=>p.ModelName=="c8030").DeepCopy();prior.Name="prior_petrus";prior.ModelName="c8901";prior.EntityID=petrus;prior.NPCParamID=890100;prior.ThinkParamID=-1;prior.CollisionName=floor.Name;target.Parts.Enemies.Add(prior);var priorModel=(MSBB.Model.Enemy)nativeMap.Models.Enemies.Single(m=>m.Name=="c8030").DeepCopy();priorModel.Name="c8901";target.Models.Enemies.Add(priorModel);
        var triangles=new[]{new MapCollision.Triangle(new(-1000,-1000,-1000),new(1000,-1000,-1000),new(0,-1000,1000))};var chunks=new[]{new MapCollisionBindings.Chunk(sourceMap.Parts.Enemies.Single(p=>p.EntityID==6070).CollisionName,triangles,triangles)};
        var script=new EMEVD(EMEVD.Game.Bloodborne);script.Events.Add(new(0));script.Events.Add(new(50));var sentinel=new EMEVD.Event(12416000);sentinel.Instructions.Add(new(1001,0,2f));script.Events.Add(sentinel);
        var initialized=MapNpcQuestState.Initialize(script.Write(),Source("event/common.emevd.dcx")!);initialized=MapQuestState.Availability(initialized,new Dictionary<int,int>{{6080,petrus},{6270,2419700},{6060,2419702}},Source("event/m10_02_00_00.emevd.dcx")!,thorolund:true).Script;var offset=new Vector3(-44.8f,20.07f,74.63f);
        var existingTalk=BND4.Read(Composed("script/talk/m40_02_00_00.talkesdbnd.dcx")!);
        Check(new[]{402970,402971,402972,402975,402980}.All(id=>existingTalk.Files.Count(f=>Leaf(f.Name)=="t"+id+".esd")==1),"Actual generic NPC/merchant/Griggs baseline missing");
        var result=MapThorolundQuest.Apply(Source,Native,Composed,sourceMap,target,offset,chunks,[floor],definitions,nativeRoot,"m40_02_00_00",initialized,petrus);
        Check(result.Actors.Select(a=>a.Talk).SequenceEqual(new[]{402981,402982,402983,402984}),"Owned Thorolund talk reservations changed");Check(sourceBefore.SequenceEqual(sourceMap.Write()),"Source MSB mutated");var map=MSBB.Read(target.Write());Check(map.Parts.Enemies.Count==4&&map.Parts.Enemies.Count(p=>p.EntityID==petrus)==1,"Duplicate Petrus or party actor");
        var actions=new SourcePlayerActions(Source);int comparisons=0;float maxError=0;
        foreach(var a in result.Actors) {
            var from=sourceMap.Parts.Enemies.Single(p=>p.EntityID==a.SourceEntity);var actor=map.Parts.Enemies.Single(p=>p.EntityID==a.Entity);
            Check(actor.ModelName==a.Model&&actor.NPCParamID==a.Npc&&actor.TalkID==a.Talk&&actor.ThinkParamID==-1&&actor.CharaInitID==-1&&actor.Position==from.Position+offset&&actor.Rotation==from.Rotation&&actor.CollisionName==floor.Name,"Source placement/owned identity mismatch");
            var appearance=MapThorolundAppearance.Load(Source,definitions,a.SourceEntity);Check(appearance.FaceGenID==a.SourceEntity&&appearance.Parts.Any(p=>p.Slot=="Face")&&appearance.Parts.Any(p=>p.Slot=="Neck"),"Actual FaceGen face/neck missing");
            Check(appearance.OpenHood==(a.SourceEntity is 6070 or 6100)&&appearance.Parts.Any(p=>p.Slot=="Hair")== (a.SourceEntity is 6080 or 6090),"Hood/default hair ownership differs");
            var flver=FLVER2.Read(BND4.Read(result.Files["chr/"+a.Model+".chrbnd.dcx"]).Files.Single(f=>f.ID==200).Bytes);Check(flver.Meshes.All(m=>m.Vertices.All(v=>float.IsFinite(v.Position.LengthSquared())&&float.IsFinite(v.Normal.LengthSquared()))),"Nonfinite face/armor geometry");
            var skin=SkinRetarget.Bake(appearance.Model.Write(),BND4.Read(Native("chr/c8030.chrbnd.dcx")).Files.Single(f=>f.ID==200).Bytes,appearance.Textures,textureOwner:a.Model,sourceProfile:"DS1R",preserveSourceDimensions:true);
            var pose=actions.Resolve(a.SourceEntity==6070?7880:0);Check(a.Pose.Source==pose.ID&&a.Pose.Physical==pose.MotionID&&a.Pose.Archive==pose.Archive&&a.Pose.Approximation==(a.SourceEntity!=6070),"Authored kneeling/explicit approximate idle contract changed");
            var bank=BND4.Read(result.Files["chr/"+a.Model+".anibnd.dcx"]);var rig=NativeAnimation.Skeleton(bank.Files.Single(f=>f.ID==1000000).Bytes.ToArray());
            foreach(int id in new[]{0,1000000,2000000}) {
                var raw=bank.Files.Single(f=>f.ID==1000000000+id).Bytes.ToArray();var pack=new NativeAnimation.Pack(raw);int anim=pack.Object("hkaSplineCompressedAnimation");Check(pack.Int(anim+56)==pose.Clip.FrameCount&&pack.Float(anim+16)==pose.Clip.Duration,"Source pose timing changed");var data=pack.Array(anim+152,1,32*1024*1024);var tracks=Spline.ReadSplineCompressedAnimByteBlock(false,raw.AsSpan(pack.Data+data.At,data.Count).ToArray(),rig.Names.Length,pack.Int(anim+60));
                foreach(int frame in new[]{0,(pose.Clip.FrameCount-1)/2,pose.Clip.FrameCount-1}) {
                    var actual=NativeAnimation.Worlds(tracks[0].Select(t=>Sample(t,frame)).ToArray(),rig.Parents);var expected=NativeAnimation.Worlds(pose.Clip.Sample(frame*pose.Clip.FrameDuration),pose.Clip.Bones.Select(b=>b.Parent).ToArray());
                    foreach(var bone in skin.Bones.Where(b=>b.Direct)){int si=pose.Clip.Bones.ToList().FindIndex(b=>b.Name==bone.Source),ti=Array.IndexOf(rig.Names,bone.Target);if(si<0||ti<0)continue;var m=expected[si];m.M41/=actor.Scale.X;m.M42/=actor.Scale.X;m.M43/=actor.Scale.X;float error=Error(actual[ti],m);maxError=Math.Max(maxError,error);Check(error<.007f,"Source pose differs at "+a.SourceEntity+"/"+id+"/"+frame+"/"+bone.Target);comparisons++;}
                }
            }
            Check(TAE.Read(bank.Files.Single(f=>f.ID==3000000).Bytes).Animations.All(a=>a.Events.Count==0),"Doll effects retained");
        }
        Check(comparisons>1000,"Source pose fixture vacuous");
        var beforeGame=BND4.Read(Composed("param/gameparam/gameparam.parambnd.dcx")!);var afterGame=BND4.Read(result.Files["param/gameparam/gameparam.parambnd.dcx"]);
        foreach(var member in beforeGame.Files){var next=afterGame.Files.Single(f=>f.ID==member.ID);if(Leaf(member.Name) is not("NpcParam.param" or "TalkParam.param"))Check(member.Bytes.Span.SequenceEqual(next.Bytes.Span),"Unrelated parameters changed");else {bool npc=Leaf(member.Name)=="NpcParam.param";var old=npc?MapCharacters.NativeRows(member.Bytes,false):ImportParam.Read(member.Bytes,"TALK_PARAM_ST",68,3);var now=npc?MapCharacters.NativeRows(next.Bytes,false):ImportParam.Read(next.Bytes,"TALK_PARAM_ST",68,3);for(int j=0;j<old.Rows.Count;j++)Check(old.Rows[j].ID==now.Rows[j].ID&&old.Rows[j].Name==now.Rows[j].Name&&ImportParam.Payload(old.Rows[j]).SequenceEqual(ImportParam.Payload(now.Rows[j])),"Prior parameter payload/name/order changed");}}
        var talk=BND4.Read(result.Files["script/talk/m40_02_00_00.talkesdbnd.dcx"]);foreach(var entry in BND4.Read(Composed("script/talk/m40_02_00_00.talkesdbnd.dcx")!).Files)Check(entry.Bytes.Span.SequenceEqual(talk.Files.Single(f=>f.ID==entry.ID).Bytes.Span),"Prior ESD changed");
        foreach(var(path,bytes)in result.Files.Where(p=>p.Key.StartsWith("msg/",StringComparison.Ordinal))){var before=BND4.Read(Composed(path)!);var after=BND4.Read(bytes);foreach(var f in before.Files){var next=after.Files.Single(n=>n.ID==f.ID);if(f.ID!=1)Check(f.Bytes.Span.SequenceEqual(next.Bytes.Span),"Unrelated localized FMG changed");else {var a=FMG.Read(f.Bytes);var b=FMG.Read(next.Bytes);Check(a.Entries.All(e=>b.Entries.Single(n=>n.ID==e.ID).Text==e.Text),"Prior subtitles changed");}}}
        var flags=new Dictionary<int,bool>();var actors=new Dictionary<int,bool>{{petrus,true}};var dead=new Dictionary<int,bool>();var events=EMEVD.Read(result.Script);
        Check(events.Events.Single(e=>e.ID==MapThorolundEvents.FirstEvent+1).RestBehavior==EMEVD.Event.RestBehaviorType.Default,"Rest substitutes source reload timing");
        Check(events.Events.Single(e=>e.ID==12416000).Instructions.Single().ArgData.SequenceEqual(sentinel.Instructions.Single().ArgData),"Prior events changed");
        Machine Event(int n)=>new(events.Events.Single(e=>e.ID==MapThorolundEvents.FirstEvent+n).Instructions,flags,actors,dead,()=>(40,2));
        void Pre()=>new Machine(events.Events.Single(e=>e.ID==50).Instructions,flags,actors,dead,()=>(40,2)).Run();
        Pre();Check(flags[F(1170)]&&flags[F(1210)]&&flags[F(1220)]&&flags[F(1202)]&&result.Actors.Where(a=>a.SourceEntity!=6080).All(a=>!actors[a.Entity]),"Fresh quest spawned late party");
        var arrive=Event(0);var stage=Event(1);Check(!arrive.Run()&&!stage.Run(),"Arrival/departure advanced without prerequisites");flags[F(11010901)]=true;Check(!arrive.Run(),"Taurus defeat substitutes Capra");flags[F(11010902)]=true;Check(arrive.Run()&&flags[F(1171)]&&flags[F(1211)]&&flags[F(1221)]&&result.Actors.All(a=>actors[a.Entity]),"Real Capra gate did not enable party");
        foreach(var dialogue in result.Dialogues) {
            var esd=ESD.Read(talk.Files.Single(f=>Leaf(f.Name)=="t"+dialogue.Talk+".esd").Bytes);var states=esd.StateGroups[1];
            int? Choose(int state,bool ended=false,bool action=true,bool isDead=false,double distance=1)=>states[state].Conditions.Where(c=>Evaluate(c.Evaluator,flags,action,ended,isDead,distance)!=0).Select(c=>(int?)c.TargetState).FirstOrDefault();
            void Enter(int state){foreach(var c in states[state].EntryCommands.Where(c=>c.CommandID==11))flags[Constant(c.Arguments[0])]=Constant(c.Arguments[1])!=0;}
            Check(Choose(1)==2,"Present NPC action unavailable");
            int first=Choose(2)??throw new Exception("Missing first speech");Check(Choose(first)==null&&Choose(first,distance:6)==3&&Choose(first,isDead:true)==3&&states[3].EntryCommands.All(c=>c.CommandID!=11),"Interrupted speech marks completion");
            int complete=Choose(first,ended:true)!.Value;Enter(complete);
            if(dialogue.SourceEntity==6070)Check(Choose(2)==102&&flags[F(71020080)],"Rhea introduction/repeat lost");
            if(dialogue.SourceEntity==6090){Check(Choose(2)==102&&flags[F(71310023)],"Vince initial mission lost");Enter(103);Check(flags[F(71310024)]&&flags[F(11026109)],"Vince completion lost");}
            if(dialogue.SourceEntity==6100)Check(flags[F(71310032)]&&flags[F(11026110)],"Nico greeting completion lost");
            if(dialogue.SourceEntity==6080){Check(Choose(2)==102,"Petrus warm greeting lost");Enter(103);Check(!flags.GetValueOrDefault(F(11020599)),"Truncated warm greeting grants fake copper coin");Check(Choose(2)==106,"Petrus arrival farewell lost");Enter(107);Check(flags[F(11020600)]&&flags[F(71300083)],"Petrus farewell source flags missing");}
            Check(states.Values.SelectMany(s=>s.EntryCommands).All(c=>c.CommandID is not(17 or 19 or 22)),"Unported miracle/covenant menu exposed");
        }
        Check(stage.Run()&&flags[F(11020560)]&&!flags.GetValueOrDefault(F(11020693))&&result.Actors.All(a=>actors[a.Entity]),"First farewell despawned party before reload");
        var saved=JsonSerializer.Deserialize<Dictionary<int,bool>>(JsonSerializer.Serialize(flags))!;flags.Clear();foreach(var f in saved)flags[f.Key]=f.Value;Pre();Check(Event(1).Run()&&flags[F(1173)]&&flags[F(1192)]&&flags[F(1216)]&&flags[F(1226)]&&flags[F(11020693)]&&flags[F(11020692)]&&result.Actors.All(a=>!actors[a.Entity]),"Reload did not perform source departure/Petrus absence");
        Pre();Check(Event(1).Run()&&flags[F(1193)]&&!flags[F(1192)]&&actors[petrus]&&result.Actors.Where(a=>a.SourceEntity!=6080).All(a=>!actors[a.Entity]),"Petrus return resurrected party or failed");
        var pDialogue=result.Dialogues.Single(d=>d.SourceEntity==6080);var pStates=ESD.Read(talk.Files.Single(f=>Leaf(f.Name)=="t"+pDialogue.Talk+".esd").Bytes).StateGroups[1];Check(pStates[2].Conditions.First(c=>Evaluate(c.Evaluator,flags)!=0).TargetState==108,"Returned Petrus uses wrong quest dialogue");
        Check(!flags.GetValueOrDefault(F(1175))&&!flags.GetValueOrDefault(F(11300005)),"Local quest fabricated Tomb rescue/Pinwheel introduction");
        flags.Clear();actors.Clear();dead.Clear();Pre();arrive=Event(0);flags[F(11010902)]=true;arrive.Run();var watcher=Event(2);watcher.Run();dead[4026070]=true;Check(Event(3).Run()&&flags[F(1177)]&&!flags[F(1171)],"Rhea death does not persist");watcher.Run();Check(actors[4026070],"Death removed source corpse immediately");Pre();Check(!actors[4026070]&&!Event(0).Run(),"Reload resurrected Rhea");flags[F(11020600)]=true;Check(!Event(1).Run()&&!flags.GetValueOrDefault(F(11020693)),"Dead party falsely progresses departure");
        flags.Clear();actors.Clear();dead.Clear();Pre();dead[petrus]=true;Check(Event(8).Run()&&flags[F(1198)]&&!flags[F(1202)],"Petrus local death did not persist1198");
        Machine PetrusVisibility()=>new(events.Events.Single(e=>e.ID==12415810).Instructions,flags,actors,dead,()=>(40,2));
        foreach(int absent in new[]{1196,1198}) {
            flags.Clear();actors.Clear();dead.Clear();Pre();flags[F(1202)]=false;flags[F(absent)]=true;Pre();Check(!actors[petrus],"Source Petrus death/absence not restored on reload:"+absent);PetrusVisibility().Run();Check(!actors[petrus],"Live availability re-enabled absent/dead Petrus:"+absent);Event(1).Run();Check(!flags.GetValueOrDefault(F(1193))&&!actors[petrus],"Local return resurrected Petrus:"+absent);
        }
        foreach(var(path,hash)in hashes)Check(Hash(File.ReadAllBytes(path))==hash,"Source/native/staged input changed: "+path);
        foreach(var(path,bytes)in result.Files){string final=Path.Combine(output,"dvdroot_ps4",path);Directory.CreateDirectory(Path.GetDirectoryName(final)!);File.WriteAllBytes(final,bytes);}
        Directory.CreateDirectory(Path.Combine(output,"dvdroot_ps4/map/MapStudio"));File.WriteAllBytes(Path.Combine(output,"dvdroot_ps4/map/MapStudio/m40_02_00_00.msb.dcx"),map.Write(DCX.Type.DCX_DFLT_10000_44_9));Directory.CreateDirectory(Path.Combine(output,"dvdroot_ps4/event"));File.WriteAllBytes(Path.Combine(output,"dvdroot_ps4/event/m40_02_00_00.emevd.dcx"),result.Script);
        File.WriteAllText(Path.Combine(output,"report.json"),JsonSerializer.Serialize(new{evidence="Source/native serialized fixture, not native gameplay",result.Actors,result.Dialogues,result.Limitations,poseComparisons=comparisons,maxPoseError=maxError,inputArchivesUnchanged=hashes.Count},new JsonSerializerOptions{WriteIndented=true}));
        Console.WriteLine($"PASS Thorolund local quest fixture: four real source faces/armor, Rhea pose7880 and three explicit unarmed idles; {comparisons} pose comparisons max{maxError*1000:F3}mm; after-Capra arrival, original greetings/farewell, deferred departure/Petrus absence+return, death/reload/interruption, prior staged assets preserved; {hashes.Count} inputs unchanged. Native gameplay/Tomb quests not claimed.");
    }
}
