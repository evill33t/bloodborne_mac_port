// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
using Spline=StudioCore.Utilities.SplineCompressedAnimation;

static class MapDuskQuestTests {
    static void Check(bool value,string why){if(!value)throw new Exception(why);}
    static void Reject(Action action,string why){try{action();}catch(InvalidDataException){return;}throw new Exception("Accepted "+why);}
    static string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();
    static string Hash(byte[] bytes)=>Convert.ToHexStringLower(SHA256.HashData(bytes));
    static int Flag(int source)=>MapNpcQuestState.Flag(source);
    static bool Same(EMEVD.Instruction a,EMEVD.Instruction b)=>a.Bank==b.Bank&&a.ID==b.ID&&a.Layer==b.Layer&&a.ArgData.SequenceEqual(b.ArgData);
    static float Error(Matrix4x4 a,Matrix4x4 b)=>new[]{Vector3.Zero,Vector3.UnitX,Vector3.UnitY,Vector3.UnitZ}.Max(v=>Vector3.Distance(Vector3.Transform(v,a),Vector3.Transform(v,b)));
    static Matrix4x4 Sample(Spline.TransformTrack t,float frame) {
        Vector3 Vector(bool scale){var types=scale?t.Mask.ScaleTypes:t.Mask.PositionTypes;var curve=scale?t.SplineScale:t.SplinePosition;var result=scale?Vector3.One:Vector3.Zero;var stat=scale?t.StaticScale:t.StaticPosition;for(int i=0;i<3;i++){if(curve!=null)result[i]=(i==0?curve.GetValueX(frame):i==1?curve.GetValueY(frame):curve.GetValueZ(frame))??result[i];else if(types.Contains((Spline.FlagOffset)(1<<i)))result[i]=stat[i];}return result;}
        var q=t.HasSplineRotation?t.SplineRotation.GetValue(frame):t.HasStaticRotation?t.StaticRotation:Quaternion.Identity;return Matrix4x4.CreateScale(Vector(true))*Matrix4x4.CreateFromQuaternion(Quaternion.Normalize(q))*Matrix4x4.CreateTranslation(Vector(false));
    }
    static double Evaluate(byte[] code,IReadOnlyDictionary<int,bool> flags,bool action=true,bool ended=false,bool dead=false,double distance=1,int? selfStatus=null,int clientStatus=0,int choice=0,bool popupOpen=true) {
        var stack=new List<double>();
        double Pop(){var value=stack[^1];stack.RemoveAt(stack.Count-1);return value;}
        for(int i=0;i<code.Length;i++) {
            byte op=code[i];
            if(op<=0x7f)stack.Add(op-64);
            else if(op==0x82){stack.Add(BitConverter.ToInt32(code,i+1));i+=4;}
            else if(op is >=0x84 and <=0x8a) {
                int count=op-0x84;var args=new double[count];for(int j=count-1;j>=0;j--)args[j]=Pop();int function=(int)Pop();
                stack.Add(function switch{1=>distance,2 or 57=>ended?1:0,3=>selfStatus??(dead?1:0),15=>flags.GetValueOrDefault((int)args[0])?1:0,19=>clientStatus,21=>popupOpen?1:0,22=>choice,56=>action?1:0,_=>throw new Exception("Unexpected ESD function "+function)});
            } else if(op is 0x94 or 0x95 or 0x98 or 0x99) {
                double b=Pop(),a=Pop();stack.Add(op switch{0x94=>a>b?1:0,0x95=>a==b?1:0,0x98=>a!=0&&b!=0?1:0,_=>a!=0||b!=0?1:0});
            } else if(op==0xa6){Check(stack.Count>0,"Continuation requires a result");}
            else if(op==0xa1){Check(i==code.Length-1&&stack.Count==1,"Malformed condition bytecode");return Pop();}
            else throw new Exception("Unexpected ESD operator "+op);
        }
        throw new Exception("Missing ESD expression terminator");
    }
    static int Constant(byte[] code)=>code.Length==2?code[0]-64:BitConverter.ToInt32(code,1);
    sealed class Machine(IEnumerable<EMEVD.Instruction> input,Dictionary<int,bool> flags,Dictionary<int,bool> actors,Dictionary<int,bool> dead) {
        readonly EMEVD.Instruction[] code=input.ToArray();readonly Dictionary<sbyte,List<Func<bool>>> groups=[];int pc;
        internal readonly List<int> Animations=[];internal readonly List<float> Waits=[];internal readonly List<(int Actor,int Target,int Socket)> Moves=[];
        bool On(int flag)=>flags.GetValueOrDefault(flag);
        bool Group(sbyte n)=>groups.TryGetValue(n,out var members)&&(n<0?members.Any(f=>f()):members.All(f=>f()));
        internal bool Run(){for(int steps=0;pc<code.Length;steps++){Check(steps<10000,"Unbounded quest event");var i=code[pc];var b=i.ArgData;
            switch((i.Bank,i.ID)) {
                case (2004,1):break;
                case (2004,5):actors[BitConverter.ToInt32(b)]=b[4]!=0;break;
                case (1003,1):if(On(BitConverter.ToInt32(b,4))==(b[1]!=0))pc+=b[0];break;
                case (1003,2):if(On(BitConverter.ToInt32(b,4))==(b[1]!=0)){pc=code.Length;return true;}break;
                case (3,0): {
                    int flag=BitConverter.ToInt32(b,4);bool state=b[1]!=0;sbyte group=unchecked((sbyte)b[0]);
                    if(group==0){if(On(flag)!=state)return false;break;}if(!groups.TryGetValue(group,out var list))groups.Add(group,list=[]);list.Add(()=>On(flag)==state);break;
                }
                case (0,0):Check(b[0]==0,"Unexpected main condition");if(Group(unchecked((sbyte)b[2]))!=(b[1]!=0))return false;break;
                case (4,0):Check(b[0]==0,"Unexpected dead group");if(dead.GetValueOrDefault(BitConverter.ToInt32(b,4))!=(b[8]!=0))return false;break;
                case (2003,2):flags[BitConverter.ToInt32(b)]=b[4]!=0;break;
                case (2004,42):Check(b[4]==2,"Unexpected rescue move target kind");Moves.Add((BitConverter.ToInt32(b),BitConverter.ToInt32(b,8),BitConverter.ToInt32(b,12)));break;
                case (2003,18):Animations.Add(BitConverter.ToInt32(b,4));break;
                case (1001,0):Waits.Add(BitConverter.ToSingle(b));break;
                default:throw new Exception("Unexpected generated event "+i.Bank+"["+i.ID+"]");
            }pc++;
        }return true;}
    }
    static void Main(string[] args) {
        string sourceRoot=args[0],nativeRoot=args[1],definitions=args[2],output=args[3];Directory.CreateDirectory(output);var hashes=new Dictionary<string,string>();
        byte[] Read(string root,string relative){string path=Path.Combine(root,relative);var bytes=File.ReadAllBytes(path);hashes.TryAdd(path,Hash(bytes));return bytes;}
        byte[]? Source(string path)=>File.Exists(Path.Combine(sourceRoot,path))?Read(sourceRoot,path):null;
        byte[] Native(string path)=>Read(nativeRoot,path);
        byte[]? Composed(string path)=>path=="script/talk/m40_04_00_00.talkesdbnd.dcx"?Native("script/talk/m24_00_00_00.talkesdbnd.dcx"):File.Exists(Path.Combine(nativeRoot,path))?Native(path):null;
        var appearance=MapDuskAppearance.Load(Source,definitions);var files=new Dictionary<string,byte[]>();
        Check(appearance.Armor.Parts.Count==4&&appearance.Armor.Parts.All(p=>p.ModelID==9480&&p.Gender=="F")&&appearance.Face.FaceGenID==6050,"Dusk's actual equipment/face lost");
        var face=appearance.Model.Materials.Where(m=>m.Textures.Any(t=>t.Path.Contains("FG_import_6050_face",StringComparison.Ordinal))).ToArray();
        Check(face.Length>0&&appearance.Model.Meshes.Any(m=>face.Contains(appearance.Model.Materials[m.MaterialIndex])),"Open crown still has no real face");
        Check(appearance.Model.Materials.Any(m=>m.Name=="Material #27"),"Crown's authored hair omitted");
        var skin=MapCharacters.Package(appearance.Model.Write(),appearance.Textures,"DS1R","c8030","c8910",Native("chr/c8030.chrbnd.dcx"),Native("chr/c8030.anibnd.dcx"),Native("action/script/c8030.hks"),files,preserveSourceDimensions:true);
        var motions=MapDuskAnimations.Apply(Source,Native,skin,files);Check(motions.Count==6&&motions.Single(m=>m.Native==7010).Physical==8284,"Source fade alias or actual native carriers lost");
        var animation=BND4.Read(files["chr/c8910.anibnd.dcx"]);var tae=TAE.Read(animation.Files.Single(f=>f.ID==3000000).Bytes);var rig=NativeAnimation.Skeleton(animation.Files.Single(f=>f.ID==1000000).Bytes.ToArray());var actions=new SourcePlayerActions(Source);
        float maxPose=0;int poses=0;
        foreach(var motion in motions) {
            var source=actions.Resolve(motion.Source).Clip;var raw=animation.Files.Single(f=>f.ID==1000000000+motion.Native).Bytes.ToArray();var p=new NativeAnimation.Pack(raw);int a=p.Object("hkaSplineCompressedAnimation");var data=p.Array(a+152,1,32*1024*1024);
            Check(p.Int(a+56)==motion.Frames&&p.Float(a+16)==motion.Duration,"Native donor timing replaced source pose duration");
            var tracks=Spline.ReadSplineCompressedAnimByteBlock(false,raw.AsSpan(p.Data+data.At,data.Count).ToArray(),rig.Names.Length,p.Int(a+60));
            foreach(int frame in new[]{0,(motion.Frames-1)/2,motion.Frames-1}) {
                var world=NativeAnimation.Worlds(tracks[0].Select(t=>Sample(t,frame)).ToArray(),rig.Parents);var expected=NativeAnimation.Worlds(source.Sample(frame*source.FrameDuration),source.Bones.Select(b=>b.Parent).ToArray());
                foreach(var pair in skin.Bones.Where(b=>b.Direct)){int si=source.Bones.ToList().FindIndex(b=>b.Name==pair.Source),ti=Array.IndexOf(rig.Names,pair.Target);if(si<0||ti<0)continue;var authored=expected[si];authored.M41/=skin.ProportionScale;authored.M42/=skin.ProportionScale;authored.M43/=skin.ProportionScale;float error=Error(world[ti],authored);maxPose=Math.Max(maxPose,error);Check(error<.007f,"Source Dusk pose differs at "+motion.Native+"/"+pair.Target);}
                poses++;
            }
            Check(tae.Animations.Single(a=>a.ID==motion.Native).Events.All(e=>e.Type is 0 or 193),"Unconverted donor or source FX retained");
        }
        var fades=tae.Animations.Single(a=>a.ID==7010).Events.Where(e=>e.Type==193).ToArray();Check(fades.Length==2&&fades[0].StartTime==0&&Math.Abs(fades[0].EndTime-1.9666667f)<.001f&&fades[1].EndTime==2&&fades.All(e=>e.Group?.GroupType==192&&e.GetParameterBytes(false).Length==16),"Source departure fade timing/payload/native group lost");
        var originalGame=BND4.Read(Native("param/gameparam/gameparam.parambnd.dcx"));var originalTalk=BND4.Read(Composed("script/talk/m40_04_00_00.talkesdbnd.dcx")!);
        var dialogue=MapDuskDialogue.Export(Source,Native,Composed,nativeRoot,definitions,files);
        Check(dialogue.Speeches.Select(s=>s.Lines).SequenceEqual(new[]{8,4,4,4})&&dialogue.Voices.Clips.Count==17&&dialogue.SourceVoiceBank=="sound/frpg_sm12.fsb"&&dialogue.Languages>0,"Dusk source speech/voice selection changed");
        var newGame=BND4.Read(files["param/gameparam/gameparam.parambnd.dcx"]);foreach(var f in originalGame.Files.Where(f=>Leaf(f.Name)!="TalkParam.param"))Check(f.Bytes.Span.SequenceEqual(newGame.Files.Single(x=>x.ID==f.ID).Bytes.Span),"Unrelated native parameter changed");
        var beforeRows=ImportParam.Read(originalGame.Files.Single(f=>Leaf(f.Name)=="TalkParam.param").Bytes,"TALK_PARAM_ST",68,3);var afterRows=ImportParam.Read(newGame.Files.Single(f=>Leaf(f.Name)=="TalkParam.param").Bytes,"TALK_PARAM_ST",68,3);
        Check(afterRows.Rows.Count==beforeRows.Rows.Count+20,"Unexpected source speech row count");for(int i=0;i<beforeRows.Rows.Count;i++)Check(beforeRows.Rows[i].ID==afterRows.Rows[i].ID&&beforeRows.Rows[i].Name==afterRows.Rows[i].Name&&ImportParam.Payload(beforeRows.Rows[i]).SequenceEqual(ImportParam.Payload(afterRows.Rows[i])),"Original native talk row changed");
        var talk=BND4.Read(files["script/talk/m40_04_00_00.talkesdbnd.dcx"]);foreach(var f in originalTalk.Files)Check(f.Bytes.Span.SequenceEqual(talk.Files.Single(x=>x.ID==f.ID).Bytes.Span),"Unrelated native ESD changed");
        var esd=ESD.Read(talk.Files.Single(f=>Leaf(f.Name)=="t404970.esd").Bytes);var states=esd.StateGroups[1];var flags=new Dictionary<int,bool>{{Flag(1121),true}};
        long? Choose(int state,int choice=0,bool open=true,bool ended=false,double distance=1)=>states[state].Conditions.FirstOrDefault(c=>Evaluate(c.Evaluator,flags,ended:ended,distance:distance,choice:choice,popupOpen:open)!=0)?.TargetState;
        Check(Choose(2)==10&&Choose(30)==null&&Choose(30,1)==31&&Choose(30,open:false)==32,"Native prompt falls through before input or reverses yes/no");
        Check(Choose(10)==null&&Choose(10,ended:true)==11&&Choose(10,distance:6)==3,"Speech interruption/completion handshake differs");
        Check(states[3].EntryCommands.All(c=>c.CommandID!=11),"Interrupted speech writes completion");
        void Enter(int state){foreach(var c in states[state].EntryCommands.Where(c=>c.CommandID==11))flags[Constant(c.Arguments[0])]=Constant(c.Arguments[1])!=0;}
        Enter(11);Check(Choose(2)==12,"Interrupted choice must repeat the short offer");Enter(31);Check(flags[Flag(71200001)]&&Choose(2)==20&&!flags.GetValueOrDefault(Flag(11200590)),"Acceptance completion happens before speech finishes");Enter(21);Check(flags[Flag(11200590)],"Source acceptance completion missing");
        flags=new(){{Flag(1121),true},{Flag(71200000),true}};Enter(32);Check(Choose(2)==22&&!flags.GetValueOrDefault(Flag(11200591)),"Refusal completion happens before speech finishes");Enter(23);Check(flags[Flag(11200591)],"Source refusal completion missing");
        foreach(var(path,bytes)in files.Where(p=>p.Key.StartsWith("msg/",StringComparison.Ordinal))) {var old=BND4.Read(Composed(path)!);var now=BND4.Read(bytes);foreach(var f in old.Files.Where(f=>f.ID is not(1 or 30)))Check(f.Bytes.Span.SequenceEqual(now.Files.Single(n=>n.ID==f.ID).Bytes.Span),"Unrelated localized FMG changed");foreach(int id in new[]{1,30}){var oldText=FMG.Read(old.Files.Single(f=>f.ID==id).Bytes);var newText=FMG.Read(now.Files.Single(f=>f.ID==id).Bytes);Check(oldText.Entries.All(e=>newText.Entries.Single(n=>n.ID==e.ID).Text==e.Text)&&newText.Entries.Count==oldText.Entries.Count+(id==1?20:1),"Original localized messages changed");}}
        var originalScript=Native("event/m24_00_00_00.emevd.dcx");var events=MapDuskEvents.Apply(originalScript,Source("event/m12_00_00_00.emevd.dcx")!,4046050,4048200,601,motions);var emitted=EMEVD.Read(events.Script);var oldScript=EMEVD.Read(originalScript);
        foreach(var e in oldScript.Events){var after=emitted.Events.Single(n=>n.ID==e.ID);int shift=e.ID==50?5:0,append=e.ID==0?5:0;Check(after.Instructions.Count==e.Instructions.Count+shift+append&&after.Instructions.Skip(shift).Take(e.Instructions.Count).Zip(e.Instructions).All(p=>Same(p.First,p.Second)),"Original native event changed");foreach(var pair in after.Parameters.Zip(e.Parameters))Check(pair.First.InstructionIndex==pair.Second.InstructionIndex+shift&&pair.First.SourceStartByte==pair.Second.SourceStartByte&&pair.First.TargetStartByte==pair.Second.TargetStartByte&&pair.First.ByteCount==pair.Second.ByteCount,"Original native event parameter changed");}
        var actors=new Dictionary<int,bool>();var dead=new Dictionary<int,bool>();flags=new(){{Flag(1120),true}};
        Machine Event(int offset)=>new(emitted.Events.Single(e=>e.ID==MapDuskEvents.FirstEvent+offset).Instructions,flags,actors,dead);
        var initial=Event(0);Check(initial.Run()&&!actors.GetValueOrDefault(4048200),"Fresh Hydra state exposes golden golem");flags[Flag(11200801)]=true;Check(initial.Run()&&!actors.GetValueOrDefault(4048200),"Golem enables inside a visit without proven703 gate");
        var reload=Event(0);Check(!reload.Run()&&actors[4048200]&&!flags.GetValueOrDefault(Flag(11200800)),"Reload after actual Hydra defeat does not enable golem");
        var rescue=Event(1);Check(!rescue.Run()&&!actors.GetValueOrDefault(4046050),"Dusk appears before real golem death");dead[4048200]=true;Check(reload.Run()&&flags[Flag(11200800)]&&rescue.Run()&&actors[4046050],"Actual golem death fails to release Dusk");
        Check(rescue.Moves.SequenceEqual(new[]{(4046050,4048200,601)})&&rescue.Animations.SequenceEqual(new[]{7000})&&flags[Flag(1121)]&&!flags[Flag(1120)],"Source rescue placement/entry/state differs");
        var accept=Event(2);Check(!accept.Run(),"Accept advances before source speech completes");flags[Flag(11200590)]=true;Check(accept.Run()&&!actors[4046050]&&flags[Flag(1122)]&&!flags[Flag(1121)]&&accept.Animations.SequenceEqual(new[]{7001,7010})&&accept.Waits.SequenceEqual(new[]{1f,2f}),"Source accept/departure state lost");
        flags=new(){{Flag(1121),true},{Flag(11200591),true}};actors[4046050]=true;Check(Event(3).Run()&&flags[Flag(1126)]&&!actors[4046050],"Source refusal persistence lost");
        flags=new(){{Flag(1121),true}};dead[4046050]=true;Check(Event(4).Run()&&flags[Flag(1125)],"Rescue-state death is not persisted");
        var bad=Source("event/m12_00_00_00.emevd.dcx")!.ToArray();bad[^1]^=1;Reject(()=>MapDuskEvents.Apply(originalScript,bad,4046050,4048200,601,motions),"source script drift");
        var sourceMap=MSB1.Read(Source("map/MapStudio/m12_00_00_00.msb")!);var target=new MSBB();var chunks=new[]{new MapCollisionBindings.Chunk("h0017B0",[],[])};var slots=new[]{new MSBB.Part.Collision{Name="fixture_collision"}};
        Reject(()=>MapDuskQuest.Apply(Source,Native,Composed,sourceMap,target,Vector3.Zero,chunks,slots,definitions,nativeRoot,"m40_04_00_00",originalScript),"rescue quest without real golden-golem dependency");Check(target.Parts.Enemies.Count==0&&target.Models.Enemies.Count==0,"Failed quest install mutated map");
        foreach(var(path,hash)in hashes)Check(Hash(File.ReadAllBytes(path))==hash,"Original asset changed: "+path);
        files["event/m40_04_00_00.emevd.dcx"]=events.Script;foreach(var(path,bytes)in files){string full=Path.Combine(output,"package",path);Directory.CreateDirectory(Path.GetDirectoryName(full)!);File.WriteAllBytes(full,bytes);}
        File.WriteAllText(Path.Combine(output,"validation.json"),JsonSerializer.Serialize(new{status="PASS",appearance=appearance.Armor.Parts,appearance.Face.FaceGenID,motions,poses,maxPose,dialogue,events=events.Events,events.Limitations,inputs=hashes,gameplayValidated=false,goldenCombatSupplied=false},new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));
        Console.WriteLine($"PASS Dusk bounded rescue fixtures:face6050/Antiquated9480;6carriers/{poses}poses;20speechrows/17voices/{dialogue.Languages}locales;yes/no/interruption;Hydra→reload→actualgolemdeath→rescue/accept/refuse/death;{hashes.Count}originalassetsunchanged. Goldencombat/nativegameplaypending.");
    }
}
