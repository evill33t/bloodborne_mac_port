// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;

static class MapGriggsQuestTests {
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
    static void Main(string[] args) {
        string sourceRoot=args[0],nativeRoot=args[1],definitions=args[2],output=args[3];Directory.CreateDirectory(output);var hashes=new Dictionary<string,string>();var staged=new Dictionary<string,byte[]>();
        byte[] Read(string root,string path){string full=Path.Combine(root,path);var bytes=File.ReadAllBytes(full);hashes.TryAdd(full,Hash(bytes));return bytes;}
        byte[]? Source(string path)=>File.Exists(Path.Combine(sourceRoot,path))?Read(sourceRoot,path):null;
        byte[] Native(string path)=>Read(nativeRoot,path);
        byte[]? Composed(string path)=>staged.TryGetValue(path,out var bytes)?bytes:path is "script/talk/m40_03_00_00.talkesdbnd.dcx" or "script/talk/m40_02_00_00.talkesdbnd.dcx"?Native("script/talk/m24_00_00_00.talkesdbnd.dcx"):File.Exists(Path.Combine(nativeRoot,path))?Native(path):null;
        NativeEventReservations.Verify(nativeRoot,MapGriggsQuest.FirstEvent,20);
        var donorMap=MSBB.Read(Native("map/MapStudio/m21_00_00_00.msb.dcx"));var results=new List<MapGriggsQuest.Result>();var scripts=new Dictionary<bool,EMEVD>();var maps=new Dictionary<bool,MSBB>();
        foreach(bool burg in new[]{true,false}) {
            string sourceMapName=burg?"m10_01_00_00":"m10_02_00_00",targetMap=burg?"m40_03_00_00":"m40_02_00_00";
            var sourceMap=MSB1.Read(Source("map/MapStudio/"+sourceMapName+".msb")!);var before=sourceMap.Write();var target=new MSBB();
            var floorModel=(MSBB.Model.Collision)donorMap.Models.Collisions[0].DeepCopy();floorModel.Name="h800000";target.Models.Collisions.Add(floorModel);var floor=(MSBB.Part.Collision)donorMap.Parts.Collisions[0].DeepCopy();floor.Name="h800000_0000";floor.ModelName=floorModel.Name;target.Parts.Collisions.Add(floor);
            // Compose the actual generic NPC and merchant exporters first. The
            // old isolated fixture missed their owned402970..972/402975 talks.
            var baseline=new MSBB();var baselineBindings=burg?new Dictionary<int,int>{{6001,4039710},{6230,4039700}}:new Dictionary<int,int>{{6270,2419700},{6080,2419701},{6060,2419702},{6240,2419703}};
            foreach(var pair in baselineBindings) {
                string model=pair.Key switch{6001=>"c8905",6230=>"c8904",6270=>"c8900",6080=>"c8901",6060=>"c8902",_=>"c8903"};
                baseline.Parts.Enemies.Add(new MSBB.Part.Enemy{Name="fixture_"+pair.Key,ModelName=model,EntityID=pair.Value,TalkID=-1});
            }
            MapTalk.Export(baseline,"DS1R",sourceMapName,nativeRoot,definitions,Source,MapNpcQuestState.Flag,baselineBindings,(p,b)=>staged[p]=b,Composed,targetMap);
            MapMerchantTalk.Export(baseline,sourceMapName,targetMap,nativeRoot,definitions,Source,Composed,baselineBindings,(p,b)=>staged[p]=b,staged);
            var baselineTalk=BND4.Read(Composed("script/talk/"+targetMap+".talkesdbnd.dcx")!);var expectedBaseline=burg?new[]{403970,403975}:new[]{402970,402971,402972,402975};
            Check(expectedBaseline.All(id=>baselineTalk.Files.Count(f=>Leaf(f.Name)=="t"+id+".esd")==1),"Actual generic NPC/merchant baseline missing");
            int sourceEntity=burg?6040:6041;var from=sourceMap.Parts.Enemies.Single(p=>p.EntityID==sourceEntity);var triangles=new[]{new MapCollision.Triangle(new(-1000,-1000,-1000),new(1000,-1000,-1000),new(0,-1000,1000))};var chunks=new[]{new MapCollisionBindings.Chunk(from.CollisionName,triangles,triangles)};
            var script=new EMEVD(EMEVD.Game.Bloodborne);script.Events.Add(new(0));script.Events.Add(new(50));var sentinel=new EMEVD.Event(12416000);sentinel.Instructions.Add(new(1001,0,2f));script.Events.Add(sentinel);
            var oldGame=Composed("param/gameparam/gameparam.parambnd.dcx")!;var oldTalk=Composed("script/talk/"+targetMap+".talkesdbnd.dcx")!;var oldMessages=Directory.EnumerateDirectories(Path.Combine(nativeRoot,"msg")).Select(Path.GetFileName).Where(n=>File.Exists(Path.Combine(nativeRoot,"msg/"+n+"/menu.msgbnd.dcx"))).ToDictionary(n=>"msg/"+n+"/menu.msgbnd.dcx",n=>Composed("msg/"+n+"/menu.msgbnd.dcx")!);
            var offset=new Vector3(-143.8f,20.07f,164.63f);var result=MapGriggsQuest.Apply(Source,Native,Composed,sourceMap,target,offset,chunks,[floor],definitions,nativeRoot,sourceMapName,targetMap,script.Write());
            Check(result.Dialogue.Talk==(burg?403980:402980),"Owned Griggs talk reservation changed");Check(before.SequenceEqual(sourceMap.Write()),"Source MSB mutated");var installed=MSBB.Read(target.Write());var actor=installed.Parts.Enemies.Single();Check(actor.EntityID==result.Entity&&actor.ModelName==result.Model&&actor.NPCParamID==result.Npc&&actor.ThinkParamID==-1&&actor.CharaInitID==-1&&actor.TalkID==result.Dialogue.Talk&&actor.Position==from.Position+offset&&actor.Rotation==from.Rotation&&actor.CollisionName==floor.Name,"Source placement/interaction frame differs");
            Check(result.FaceGenID==6040&&result.Appearance.Single(p=>p.Slot=="Head").ModelID==9372&&result.Appearance.All(p=>p.RequestedID is 640004 or 641004 or 642004 or 643004),"Source Griggs equipment differs");
            var flver=FLVER2.Read(BND4.Read(result.Files["chr/"+result.Model+".chrbnd.dcx"]).Files.Single(f=>f.ID==200).Bytes);Check(flver.Materials.Any(m=>m.Textures.Any(t=>t.Path.Contains("FG_import_6040_face",StringComparison.Ordinal))),"Open hat has no actual Griggs face");Check(flver.Meshes.All(m=>m.Vertices.All(v=>float.IsFinite(v.Position.LengthSquared())&&float.IsFinite(v.Normal.LengthSquared()))),"Nonfinite appearance");
            var anim=BND4.Read(result.Files["chr/"+result.Model+".anibnd.dcx"]);foreach(int id in new[]{0,1000000,2000000}){var p=new NativeAnimation.Pack(anim.Files.Single(f=>f.ID==1000000000+id).Bytes.ToArray());Check(p.Int(p.Object("hkaSplineCompressedAnimation")+56)==result.Pose.Frames,"Source idle carrier frame count differs");}Check(TAE.Read(anim.Files.Single(f=>f.ID==3000000).Bytes).Animations.All(a=>a.Events.Count==0),"Doll events retained");
            var beforeGame=BND4.Read(oldGame);var afterGame=BND4.Read(result.Files["param/gameparam/gameparam.parambnd.dcx"]);foreach(var member in beforeGame.Files){var next=afterGame.Files.Single(f=>f.ID==member.ID);if(Leaf(member.Name) is not("NpcParam.param" or "TalkParam.param"))Check(member.Bytes.Span.SequenceEqual(next.Bytes.Span),"Unrelated global table overwritten");else {bool npc=Leaf(member.Name)=="NpcParam.param";var prior=npc?MapCharacters.NativeRows(member.Bytes,false):ImportParam.Read(member.Bytes,"TALK_PARAM_ST",68,3);var nextRows=npc?MapCharacters.NativeRows(next.Bytes,false):ImportParam.Read(next.Bytes,"TALK_PARAM_ST",68,3);Check(nextRows.Rows.Count>=prior.Rows.Count,"Prior parameter row removed");for(int j=0;j<prior.Rows.Count;j++)Check(nextRows.Rows[j].ID==prior.Rows[j].ID&&nextRows.Rows[j].Name==prior.Rows[j].Name&&ImportParam.Payload(nextRows.Rows[j]).SequenceEqual(ImportParam.Payload(prior.Rows[j])),"Prior staged/native parameter payload/order/name changed");}}
            var talkBank=BND4.Read(result.Files["script/talk/"+targetMap+".talkesdbnd.dcx"]);foreach(var member in BND4.Read(oldTalk).Files)Check(member.Bytes.Span.SequenceEqual(talkBank.Files.Single(f=>f.ID==member.ID).Bytes.Span),"Prior talk overwritten");
            foreach(var(path,oldBytes)in oldMessages){var old=BND4.Read(oldBytes);var now=BND4.Read(result.Files[path]);foreach(var member in old.Files){var next=now.Files.Single(f=>f.ID==member.ID);if(member.ID!=1)Check(member.Bytes.Span.SequenceEqual(next.Bytes.Span),"Unrelated locale text changed");else {var a=FMG.Read(member.Bytes);var b=FMG.Read(next.Bytes);Check(a.Entries.All(e=>b.Entries.Single(n=>n.ID==e.ID).Text==e.Text),"Prior staged/native subtitle overwritten");}}}
            var esd=ESD.Read(talkBank.Files.Single(f=>Leaf(f.Name)=="t"+result.Dialogue.Talk+".esd").Bytes);var states=esd.StateGroups[1];var flags=new Dictionary<int,bool>{{F(burg?1110:1112),true}};
            int? Choose(int state,bool action=true,bool ended=false,double distance=1)=>states[state].Conditions.Where(c=>Evaluate(c.Evaluator,flags,action,ended,distance:distance)!=0).Select(c=>(int?)c.TargetState).FirstOrDefault();
            void Enter(int state){foreach(var c in states[state].EntryCommands.Where(c=>c.CommandID==11))flags[Constant(c.Arguments[0])]=Constant(c.Arguments[1])!=0;}
            if(burg){Check(Choose(1,action:false,distance:7)==null&&Choose(1,action:false,distance:5.5)==10&&Choose(10,distance:5.5)==null&&Choose(10,ended:true)==11,"Authentic automatic plea range/completion failed");Enter(11);Check(Choose(1)==null&&flags[F(71010035)],"Locked Griggs became a rescue conversation");flags[F(1110)]=false;flags[F(1111)]=true;Check(Choose(1)==2&&Choose(2)==20&&Choose(20)==null&&Choose(20,ended:true)==21,"Rescue thanks handshake failed");Enter(21);Check(Choose(2)==30&&flags[F(71010040)],"Rescue repeat flag failed");}
            else {Check(Choose(1)==2&&Choose(2)==20&&Choose(20)==null&&Choose(20,ended:true)==21,"Arrival handshake failed");Enter(21);Check(Choose(2)==30&&flags[F(71020067)],"Neutral repeat flag failed");Check(states.Values.SelectMany(s=>s.EntryCommands).All(c=>c.CommandID is not(17 or 22)),"Absent teaching service exposed");}
            Check(Choose(20,distance:6)==3&&states[3].EntryCommands.All(c=>c.CommandID!=11),"Interrupted greeting marked complete");
            var events=EMEVD.Read(result.Script);Check(events.Events.Single(e=>e.ID==12416000).Instructions.Single().ArgData.SequenceEqual(sentinel.Instructions.Single().ArgData),"Prior event changed");scripts[burg]=events;maps[burg]=installed;
            foreach(var pair in result.Files)staged[pair.Key]=pair.Value;results.Add(result);
        }
        var flagState=new Dictionary<int,bool>{{F(1110),true}};var actors=new Dictionary<int,bool>();var dead=new Dictionary<int,bool>();var current=(Area:40,Block:3);
        Machine Event(bool burg,int local)=>new(scripts[burg].Events.Single(e=>e.ID==MapGriggsQuest.FirstEvent+(burg?0:10)+local).Instructions,flagState,actors,dead,()=>current);
        new Machine(scripts[true].Events.Single(e=>e.ID==50).Instructions,flagState,actors,dead,()=>current).Run();new Machine(scripts[false].Events.Single(e=>e.ID==50).Instructions,flagState,actors,dead,()=>current).Run();Check(actors[4036040]&&!actors[4026041],"Fresh save spawned late Firelink Griggs");
        var rescue=Event(true,0);var arrival=Event(false,0);var burgPresence=Event(true,1);var firelinkPresence=Event(false,1);burgPresence.Run();Check(!firelinkPresence.Run()&&!rescue.Run()&&!arrival.Run(),"Rescue/arrival advanced without actual key door");
        flagState[F(11010181)]=true;Check(rescue.Run()&&flagState[F(1111)]&&!flagState[F(1110)]&&!arrival.Run(),"Real door did not rescue, or merely streaming Firelink advanced quest");
        current=(40,2);Check(arrival.Run()&&flagState[F(1112)]&&!flagState[F(1111)],"Native Firelink entry not adapted");burgPresence.Run();firelinkPresence.Run();Check(!actors[4036040]&&actors[4026041],"Cross-map NPC duplicated/disappeared");
        var saved=JsonSerializer.Deserialize<Dictionary<int,bool>>(JsonSerializer.Serialize(flagState))!;Check(saved[F(1112)]&&!saved.GetValueOrDefault(F(1110)),"Saved quest stage differs");flagState.Clear();foreach(var f in saved)flagState[f.Key]=f.Value;
        new Machine(scripts[true].Events.Single(e=>e.ID==50).Instructions,flagState,actors,dead,()=>current).Run();new Machine(scripts[false].Events.Single(e=>e.ID==50).Instructions,flagState,actors,dead,()=>current).Run();Check(!actors[4036040]&&actors[4026041],"Reload resets rescue stage");
        dead[4026041]=true;Check(Event(false,2).Run()&&flagState[F(1115)]&&!flagState[F(1112)],"NPC death not shared persistently");firelinkPresence.Run();Check(actors[4026041],"Death watcher erased corpse before reload");new Machine(scripts[false].Events.Single(e=>e.ID==50).Instructions,flagState,actors,dead,()=>current).Run();Check(!actors[4026041],"Reload respawned dead Griggs");
        Check(staged.ContainsKey("sound/sprj_c8912.fev")&&staged.ContainsKey("sound/sprj_c8913.fev"),"Second map overwrote first voice project");
        var finalRows=MapCharacters.NativeRows(BND4.Read(staged["param/gameparam/gameparam.parambnd.dcx"]).Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes,false);Check(finalRows.Rows.Any(r=>r.ID==891200)&&finalRows.Rows.Any(r=>r.ID==891300),"Second map overwrote first NPC parameters");
        foreach(var(path,expected)in hashes)Check(Hash(File.ReadAllBytes(path))==expected,"Original resource changed: "+path);
        foreach(var(path,bytes)in staged){string target=Path.Combine(output,"dvdroot_ps4",path);Directory.CreateDirectory(Path.GetDirectoryName(target)!);File.WriteAllBytes(target,bytes);}foreach(bool burg in new[]{true,false}){string map=burg?"m40_03_00_00":"m40_02_00_00";string dir=Path.Combine(output,"dvdroot_ps4/map/MapStudio");Directory.CreateDirectory(dir);File.WriteAllBytes(Path.Combine(dir,map+".msb.dcx"),maps[burg].Write(DCX.Type.DCX_DFLT_10000_44_9));dir=Path.Combine(output,"dvdroot_ps4/event");Directory.CreateDirectory(dir);File.WriteAllBytes(Path.Combine(dir,map+".emevd.dcx"),scripts[burg].Write(DCX.Type.DCX_DFLT_10000_44_9));}
        File.WriteAllText(Path.Combine(output,"report.json"),JsonSerializer.Serialize(new{evidence="Serialized resource/fixture evidence; native gameplay not run",results=results.Select(r=>new{r.SourceName,r.Model,r.Entity,r.Npc,r.FaceGenID,r.Appearance,r.Pose,r.Dialogue,r.Limitations}),originalAssetsUnchanged=hashes.Count,flags=flagState},new JsonSerializerOptions{WriteIndented=true}));
        Console.WriteLine($"PASS Griggs rescue/arrival fixture: two source placements, Face6040 and Black Sorcerer set; actual source door1110→1111→Firelink1112, absence on fresh save, cross-map residency, reload and shared death; original speech/voices/subtitles, actual generic NPC and merchant talks preserved; {hashes.Count} original archives unchanged. Source sorcery service and native gameplay remain unproved.");
    }
}
