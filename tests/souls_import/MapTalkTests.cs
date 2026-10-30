// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

static class MapTalkTests {
    static void Check(bool value,string message){if(!value)throw new Exception(message);}
    static string Leaf(string path)=>Path.GetFileName(path.Replace('\\','/'));
    static double Evaluate(byte[] code,IReadOnlyDictionary<int,bool> flags,bool action=true,bool ended=false,bool dead=false,double distance=1,int? selfStatus=null,int clientStatus=0) {
        var stack=new List<double>();
        double Pop(){var value=stack[^1];stack.RemoveAt(stack.Count-1);return value;}
        for(int i=0;i<code.Length;i++) {
            byte op=code[i];
            if(op<=0x7f)stack.Add(op-64);
            else if(op==0x82){stack.Add(BitConverter.ToInt32(code,i+1));i+=4;}
            else if(op is >=0x84 and <=0x8a) {
                int count=op-0x84;var args=new double[count];for(int j=count-1;j>=0;j--)args[j]=Pop();int function=(int)Pop();
                stack.Add(function switch{1=>distance,2 or 57=>ended?1:0,3=>selfStatus??(dead?1:0),15=>flags.GetValueOrDefault((int)args[0])?1:0,19=>clientStatus,56=>action?1:0,_=>throw new Exception("Unexpected ESD function "+function)});
            } else if(op is 0x94 or 0x95 or 0x98 or 0x99) {
                double b=Pop(),a=Pop();stack.Add(op switch{0x94=>a>b?1:0,0x95=>a==b?1:0,0x98=>a!=0&&b!=0?1:0,_=>a!=0||b!=0?1:0});
            } else if(op==0xa6){Check(stack.Count>0,"Continuation requires a result");}
            else if(op==0xa1){Check(i==code.Length-1&&stack.Count==1,"Malformed condition bytecode");return Pop();}
            else throw new Exception("Unexpected ESD operator "+op);
        }
        throw new Exception("Missing ESD expression terminator");
    }
    static int Constant(byte[] code)=>code.Length==2?code[0]-64:BitConverter.ToInt32(code,1);
    static IEnumerable<ESD.Condition> Conditions(ESD.Condition condition) {
        yield return condition;foreach(var sub in condition.Subconditions)foreach(var c in Conditions(sub))yield return c;
    }
    // This independently reads only the shipped RIFF chunk table and native
    // LGCY name records, not the production writer's graph/parser or deltas.
    static (int ProjectChars,int BankChars,int Banks,int Languages,int Mode,string FirstSuffix,byte[] LanguageMetadata,Dictionary<int,int> Objects) NameAllocations(byte[] bytes) {
        int Number(int at)=>BitConverter.ToInt32(bytes,at);
        int Chunk(int at,int end,string tag) {
            for(int p=at;p<end;){int size=Number(p+4);Check(size>=0&&p+8L+size<=end,"Invalid fixture chunk");if(Encoding.ASCII.GetString(bytes,p,4)==tag)return p;p=checked(p+8+size+(size&1));}
            throw new Exception("Fixture chunk missing: "+tag);
        }
        Check(Encoding.ASCII.GetString(bytes,0,4)=="RIFF"&&Number(4)==bytes.Length-8,"Invalid native FEV fixture");
        int list=Chunk(12,bytes.Length,"LIST"),end=list+8+Number(list+4);
        Check(Encoding.ASCII.GetString(bytes,list+8,4)=="PROJ","Missing native project");
        int legacy=Chunk(list+12,end,"LGCY"),objects=Chunk(list+12,end,"OBCT");
        int at=legacy+16,project=Number(at);at+=4+project;
        int banks=Number(at),languages=Number(at+4);at+=8;int names=0,mode=-1,suffixIndex=-1;
        Check(project>0&&banks>=0&&banks<32&&languages>0&&languages<32,"Invalid native name fixture");
        for(int i=0;i<banks;i++){if(i==0){mode=Number(at);suffixIndex=Number(at+16);}at+=8+languages*12;int n=Number(at);Check(n>0&&at+4L+n<=legacy+8+Number(legacy+4),"Invalid bank name fixture");names+=n;at+=4+n;}
        var counts=new Dictionary<int,int>();int count=Number(objects+8);
        for(int i=0;i<count;i++)counts.Add(Number(objects+12+i*8),Number(objects+16+i*8));
        string suffix="";
        if(suffixIndex>=0){int strings=Chunk(list+12,end,"STRR")+8,n=Number(strings);Check(suffixIndex<n,"Invalid suffix identity");int start=strings+4+n*4+Number(strings+4+suffixIndex*4);int stop=Array.IndexOf(bytes,(byte)0,start);suffix=Encoding.UTF8.GetString(bytes,start,stop-start);}
        int lang=Chunk(list+12,end,"LANG");var language=bytes.AsSpan(lang+8,Number(lang+4)).ToArray();
        return(project,names,banks,languages,mode,suffix,language,counts);
    }
    public static void Main(string[] args) {
        string sourceRoot=args[0],nativeRoot=args[1],defs=args[2];var hashes=new Dictionary<string,byte[]>();
        byte[]? Read(string root,string path) {
            string full=Path.Combine(root,path);if(!File.Exists(full))return null;var bytes=File.ReadAllBytes(full);
            hashes.TryAdd(full,SHA256.HashData(bytes));return bytes;
        }
        byte[] Native(string path)=>Read(nativeRoot,path)??throw new FileNotFoundException(path);
        var mixerNames=NameAllocations(Native("sound/sprj_mixer.fev"));
        Check(mixerNames.Banks==0&&mixerNames.Objects[19]==mixerNames.ProjectChars&&mixerNames.Objects[20]==0,"Retail bank-free mixer must distinguish project-name and wavebank-name allocation counters");
        int nativeNameFixtures=0;
        foreach(string path in Directory.EnumerateFiles(Path.Combine(nativeRoot,"sound"),"*.fev")) {
            var bytes=File.ReadAllBytes(path);
            // Two installed character placeholders are not complete RIFF
            // projects. They do not provide a native allocation fixture.
            if(bytes.Length<12||Encoding.ASCII.GetString(bytes,0,4)!="RIFF"||BitConverter.ToInt32(bytes,4)!=bytes.Length-8)continue;
            hashes.TryAdd(path,SHA256.HashData(bytes));var fixture=NameAllocations(bytes);
            Check(fixture.Objects[19]==fixture.ProjectChars&&fixture.Objects[20]==fixture.BankChars,"Native name allocation contract differs in "+Path.GetFileName(path));nativeNameFixtures++;
        }
        Check(nativeNameFixtures>1,"Native bank-name allocation corpus is unavailable");
        var defaultNames=NameAllocations(Native("sound/sprj_c7110.fev"));
        Check(defaultNames.Banks==1&&defaultNames.Languages==1&&defaultNames.Mode==256&&defaultNames.FirstSuffix=="","Expected native default-language PCM character project");
        var sourceGame=BND3.Read(Read(sourceRoot,"param/GameParam/GameParam.parambnd.dcx")!);var sourceRows=PARAM.Read(sourceGame.Files.Single(f=>Leaf(f.Name)=="TalkParam.param").Bytes);
        sourceRows.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(defs,"TalkParam.xml")));
        var paramDef=PARAMDEF.XmlDeserialize(Path.GetFullPath(Path.Combine(defs,"../../BB/Defs/TalkParam.xml")));
        var game=BND4.Read(Native("param/gameparam/gameparam.parambnd.dcx"));
        var talkParam=game.Files.Single(f=>Leaf(f.Name)=="TalkParam.param");var originalRows=PARAM.Read(talkParam.Bytes);originalRows.ApplyParamdef(paramDef);
        // A preceding stage may contain an opaque modification and a genuine null row name.
        originalRows.Rows[0].Name=null;talkParam.Bytes=MapTalk.WriteRows(originalRows);
        originalRows=PARAM.Read(talkParam.Bytes);originalRows.ApplyParamdef(paramDef);
        var objectFile=game.Files.Single(f=>Leaf(f.Name)=="ObjectParam.param");var opaque=objectFile.Bytes.ToArray();opaque[^1]^=1;objectFile.Bytes=opaque;
        byte[] composedGame=game.Write(game.Compression);
        var ids=new Dictionary<int,int>{{6270,2419700},{6080,2419701},{6060,2419702}};
        var flags=new Dictionary<int,int>();int Flag(int source){if(!flags.TryGetValue(source,out var target)){target=52100100+flags.Count;flags.Add(source,target);}return target;}
        var results=new List<object>();
        foreach(string nativeMap in new[]{"m24_01_00_00","m24_00_00_00"}) {
            string talkPath=$"script/talk/{nativeMap}.talkesdbnd.dcx";
            var originalTalk=BND4.Read(Native(talkPath));int initial=int.Parse(nativeMap.AsSpan(1,2))*10000+int.Parse(nativeMap.AsSpan(4,2))*1000+970;
            var donor=originalTalk.Files[0];originalTalk.Files.Add(new BinderFile(donor.Flags,originalTalk.Files.Max(f=>f.ID)+1,$"t{initial}.esd",donor.Bytes));
            byte[] composedTalk=originalTalk.Write(originalTalk.Compression);
            string textPath="msg/engus/menu.msgbnd.dcx";var textBinder=BND4.Read(Native(textPath));var textFile=textBinder.Files.Single(f=>f.ID==1);var text=FMG.Read(textFile.Bytes);
            text.Entries.Add(new FMG.Entry(text,900070000,"Existing imported text"));textFile.Bytes=text.Write();byte[] composedText=textBinder.Write(textBinder.Compression);
            byte[]? Composed(string path)=>path switch{"param/gameparam/gameparam.parambnd.dcx"=>composedGame,"msg/engus/menu.msgbnd.dcx"=>composedText,_=>path==talkPath?composedTalk:Read(nativeRoot,path)};
            int nextModel=nativeMap=="m24_01_00_00"?8900:8970;
            var map=new MSBB();foreach(var pair in ids)map.Parts.Enemies.Add(new MSBB.Part.Enemy{Name="import"+pair.Key,ModelName="c"+nextModel++,EntityID=pair.Value,TalkID=-1,ThinkParamID=-1});
            var files=new Dictionary<string,byte[]>();
            var result=MapTalk.Export(map,"DS1R","m10_02_00_00",nativeRoot,defs,p=>Read(sourceRoot,p),Flag,ids,files.Add,Composed,nativeMap);
            Check(result.Actors.Count==3&&result.TalkParamRows==45&&result.Languages>0,"Source conversation selection changed");
            Check(result.Voices.Clips.Count==41&&result.Voices.Clips.Select(c=>c.NativeVoice).Distinct().Count()==41,"Selected source voices must share one native event per original clip");
            var expectedVoices=result.Actors.SelectMany(a=>a.Branches).SelectMany(b=>Enumerable.Range(b.SourceTalkParam,b.Lines)).Select(id=>(int)sourceRows.Rows.Single(r=>r.ID==id)["voiceId"].Value).Distinct().ToArray();
            Check(result.Voices.Clips.Select(c=>c.SourceVoice).SequenceEqual(expectedVoices)&&result.Voices.Clips.Select(c=>c.NativeVoice).SequenceEqual(Enumerable.Range(MapTalkVoices.FirstVoice,41)),"Native voice IDs must remain globally stable across per-actor banks");
            Check(result.Voices.Projects.Count==3&&result.Voices.Projects.Select(p=>p.Model).SequenceEqual(result.Actors.Select(a=>map.Parts.Enemies.Single(p=>p.EntityID==a.NativeEntity).ModelName)),"Owned sound projects must follow actual actor models");
            Check(result.Voices.Projects.Select(p=>p.Clips.Count).SequenceEqual(new[]{33,7,1}),"Source clips were assigned to the wrong actor");
            var expectedSoundPaths=result.Voices.Projects.SelectMany(p=>new[]{"sound/"+p.BankName+".fev","sound/"+p.BankName+".fsb"}).ToHashSet(StringComparer.Ordinal);
            Check(files.Keys.Where(p=>p.StartsWith("sound/",StringComparison.Ordinal)).ToHashSet().SetEquals(expectedSoundPaths),"Only owned FEV/base FSB pairs may be exported; retail sound assets must remain unchanged");
            foreach(var project in result.Voices.Projects) {
                var owned=files[$"sound/{project.BankName}.fev"];var exportedNames=NameAllocations(owned);
                int nameBytes=Encoding.UTF8.GetByteCount(project.BankName)+1;
                Check(exportedNames.Objects[19]==nameBytes&&exportedNames.ProjectChars==nameBytes&&exportedNames.Objects[20]==nameBytes&&exportedNames.BankChars==nameBytes,"Owned project/wavebank names are not fully allocated");
                Check(exportedNames.Banks==1&&exportedNames.Languages==1&&exportedNames.Mode==defaultNames.Mode&&exportedNames.FirstSuffix==""&&exportedNames.LanguageMetadata.SequenceEqual(defaultNames.LanguageMetadata),"Owned character banks must retain native default-language metadata");
                Check(exportedNames.Objects[11]==project.Clips.Count&&exportedNames.Objects[13]==project.Clips.Count&&exportedNames.Objects[17]==project.Clips.Count&&exportedNames.Objects[26]==27*project.Clips.Count,"Owned voice graph allocations changed");
                Check(Convert.ToHexString(SHA256.HashData(owned))==project.FevSha256,"Owned project report hash mismatch");
                var voiceBank=files[$"sound/{project.BankName}.fsb"];int voiceHeaders=BitConverter.ToInt32(voiceBank,12),voiceNames=BitConverter.ToInt32(voiceBank,16),voiceData=60+voiceHeaders+voiceNames;
                Check(BitConverter.ToInt32(voiceBank,8)==project.Clips.Count&&BitConverter.ToInt32(voiceBank,24)==2,"Expected an owned PCM16 native voice bank");
                Check(project.Clips.Select(c=>c.SampleIndex).SequenceEqual(Enumerable.Range(0,project.Clips.Count)),"Sample indices must restart inside each owned FSB");
                foreach(var clip in project.Clips) {
                    ulong mode=BitConverter.ToUInt64(voiceBank,60+clip.SampleIndex*8);int position=checked(voiceData+(int)((mode>>7)&0x7ffffff)*32),frames=checked((int)(mode>>34));
                    Check(frames==clip.Frames&&clip.Channels==1&&clip.Rate==44100&&(mode&1)==0&&((mode>>1)&15)==8&&((mode>>5)&3)==0,"Native PCM sample framing/rate changed");
                    Check(Convert.ToHexString(SHA256.HashData(voiceBank.AsSpan(position,frames*2)))==clip.PcmSha256,"Serialized native PCM differs from decoded source speech");
                }
                Check(Convert.ToHexString(SHA256.HashData(voiceBank))==project.FsbSha256,"Owned FSB report hash mismatch");
            }
            Check(files.ContainsKey(talkPath)&&result.Actors[0].NativeTalkId==initial+1,"Target map or occupied TalkID was ignored");
            var newGame=BND4.Read(files["param/gameparam/gameparam.parambnd.dcx"]);
            foreach(var file in game.Files.Where(f=>Leaf(f.Name)!="TalkParam.param"))Check(file.Bytes.Span.SequenceEqual(newGame.Files.Single(f=>f.ID==file.ID).Bytes.Span),"Composed unrelated parameter changed");
            var newRows=PARAM.Read(newGame.Files.Single(f=>Leaf(f.Name)=="TalkParam.param").Bytes);newRows.ApplyParamdef(paramDef);
            Check(newRows.Rows.Count==originalRows.Rows.Count+45&&newRows.Rows[0].Name==null,"Original row count/null name was not preserved");
            for(int i=0;i<originalRows.Rows.Count;i++) {
                var before=originalRows.Rows[i];var after=newRows.Rows[i];
                Check(before.ID==after.ID&&before.Name==after.Name,$"Original TalkParam identity changed at {i}: {before.ID}/{after.ID}, '{before.Name}'/'{after.Name}'");
                for(int c=0;c<before.Cells.Count;c++)Check(Equals(before.Cells[c].Value,after.Cells[c].Value),$"Original TalkParam row {before.ID} field {before.Cells[c].Def.InternalName} changed: {before.Cells[c].Value}/{after.Cells[c].Value}");
            }
            var newTalk=BND4.Read(files[talkPath]);foreach(var file in originalTalk.Files)Check(file.Bytes.Span.SequenceEqual(newTalk.Files.Single(f=>f.ID==file.ID).Bytes.Span),"Original/composed ESD changed");
            foreach(var path in files.Keys.Where(p=>p.StartsWith("msg/",StringComparison.Ordinal))) {
                var before=BND4.Read(Composed(path)!);var after=BND4.Read(files[path]);
                foreach(var file in before.Files.Where(f=>f.ID!=1))Check(file.Bytes.Span.SequenceEqual(after.Files.Single(f=>f.ID==file.ID).Bytes.Span),"Unrelated menu FMG changed");
                var oldText=FMG.Read(before.Files.Single(f=>f.ID==1).Bytes);var newText=FMG.Read(after.Files.Single(f=>f.ID==1).Bytes);
                Check(newText.Entries.Count==oldText.Entries.Count+45&&oldText.Entries.All(e=>newText.Entries.Single(n=>n.ID==e.ID).Text==e.Text),"Original conversation text changed");
            }
            foreach(var actor in result.Actors) {
                Check(map.Parts.Enemies.Single(p=>p.EntityID==actor.NativeEntity).TalkID==actor.NativeTalkId,"Actor was not bound to its unique ESD");
                var esd=ESD.Read(newTalk.Files.Single(f=>Leaf(f.Name)==$"t{actor.NativeTalkId}.esd").Bytes);var states=esd.StateGroups[1];
                Check(esd.LongFormat&&esd.DarkSoulsCount==2&&esd.StateGroups.Count==1,"Native ESD layout changed");
                Check(esd.Name==$"t{actor.NativeTalkId}","Native ESD identity must equal its unique filename stem");
                var empty=new Dictionary<int,bool>();
                Check(states[1].Conditions[0].Evaluator.SequenceEqual(new byte[]{0x78,0x82,0x70,0x17,0,0,0x85,0xa6,0xa1})&&Evaluate(states[1].Conditions[0].Evaluator,empty)==1&&Evaluate(states[1].Conditions[0].Evaluator,empty,action:false)==0,"Native action-button interaction gate changed");
                foreach(var branch in actor.Branches) {
                    int index=actor.Branches.ToList().IndexOf(branch);var speech=states[100+2*index];var completion=states[101+2*index];
                    Check(Constant(speech.EntryCommands.Single().Arguments[0])==branch.NativeTalkParam,"Speech root does not match source selection");
                    Check(Evaluate(speech.Conditions[1].Evaluator,empty)==0&&Evaluate(speech.Conditions[1].Evaluator,empty,ended:true)==1,"Progress must wait for conversation completion");
                    Check(Evaluate(speech.Conditions[0].Evaluator,empty,distance:6)==1&&states[3].EntryCommands.All(c=>c.CommandID!=11),"Interrupted conversation must not write progression");
                    Check(Evaluate(speech.Conditions[0].Evaluator,empty,selfStatus:-1,clientStatus:-1)==0&&Evaluate(speech.Conditions[0].Evaluator,empty,dead:true)==1,"Unknown native status values must not abort as truthy");
                    Check(speech.Conditions[1].Evaluator.SequenceEqual(new byte[]{0x79,0x40,0x85,0xa6,0x41,0x95,0xa1})&&states[3].EntryCommands[0].CommandID==71,"Native Doll conversation completion/Havok notification changed");
                    Check(completion.EntryCommands.Select(c=>Constant(c.Arguments[0])).SequenceEqual(branch.NativeCompletionFlags),"Completion writes unexpected quest flags");
                    foreach(var row in newRows.Rows.Where(r=>r.ID>=branch.NativeTalkParam&&r.ID<branch.NativeTalkParam+branch.Lines))
                        Check(result.Voices.Clips.Any(c=>c.NativeVoice==(int)row["voiceId"].Value)&&(int)row["eventId"].Value==-1&&(int)row["returnPos"].Value==0,"Source speech is not registered in the isolated native bank");
                }
                long Choose(Dictionary<int,bool> state)=>states[2].Conditions.First(c=>Evaluate(c.Evaluator,state)!=0).TargetState??throw new Exception("Missing branch target");
                long Target(string name)=>100+2*actor.Branches.ToList().FindIndex(b=>b.Name==name);
                if(actor.SourceEntity==6270) {
                    // Compare a shipped flag-OFF expression independently of the
                    // test evaluator; only substitute the allocated flag operand.
                    var retailOff=originalTalk.Files.SelectMany(f=>ESD.Read(f.Bytes).StateGroups.Values).SelectMany(g=>g.Values).SelectMany(s=>s.Conditions).SelectMany(Conditions).Select(c=>c.Evaluator)
                        .First(b=>b.Length==11&&b[0]==0x4f&&b[1]==0x82&&b[6]==0x85&&b[7]==0xa6&&b[8]==0x40&&b[9]==0x95&&b[10]==0xa1).ToArray();
                    BitConverter.GetBytes(Flag(71100026)).CopyTo(retailOff,2);
                    Check(states[2].Conditions.Single(c=>c.TargetState==Target("introduction")).Evaluator.SequenceEqual(retailOff),"Generated flag-OFF condition differs from native continuation/compare encoding");
                    Check(Choose(empty)==Target("introduction"),"Fresh Crestfallen introduction lost");
                    var progress=new Dictionary<int,bool>{{Flag(71100026),true}};Check(Choose(progress)==Target("bell directions"),"Crestfallen continuation lost");
                    progress[Flag(71100027)]=true;Check(Choose(progress)==Target("questions exhausted"),"Crestfallen exhaustion lost");
                    progress[Flag(71100028)]=true;Check(Choose(progress)==Target("repeat"),"Crestfallen repeat lost");
                    progress[Flag(11010700)]=true;Check(Choose(progress)==Target("upper bell"),"Upper-bell branch lost");
                    progress[Flag(11400200)]=true;Check(Choose(progress)==Target("both bells"),"Both-bells branch lost");
                    progress[Flag(71100049)]=true;Check(Choose(progress)==Target("repeat"),"Unported departure was exposed");
                    Check(Choose(new Dictionary<int,bool>{{Flag(11400200),true}})==Target("lower bell"),"Lower-bell branch lost");
                } else if(actor.SourceEntity==6080) {
                    Check(Choose(empty)==Target("introduction"),"Fresh Petrus introduction lost");var progress=new Dictionary<int,bool>{{Flag(71300065),true}};
                    Check(Choose(progress)==Target("warm greeting"),"Petrus continuation lost");progress[Flag(71300066)]=true;Check(Choose(progress)==Target("repeat"),"Petrus repeat lost");
                    Check(actor.Branches.Sum(b=>b.Lines)==7&&actor.Branches.All(b=>!b.SourceCompletionFlags.Contains(11020599)),"Unported gift or miracle promise leaked");
                } else Check(Choose(empty)==Target("silent greeting"),"Anastacia source silence lost");
            }
            Check(result.Actors.SelectMany(a=>a.Branches).All(b=>b.NativeTalkParam>=900070100),"Text allocator overwrote a composed entry");
            results.Add(new{nativeMap,result,Files=files.Count});
        }
        int rejectedWrites=0;void Reject(Action action){bool rejected=false;try{action();}catch(InvalidDataException){rejected=true;}Check(rejected&&rejectedWrites==0,"Invalid/occupied voice ownership must reject before output writes");}
        Reject(()=>MapTalkVoices.Export([new("c9000",[37000000])],p=>Read(sourceRoot,p),p=>Read(nativeRoot,p),(p,b)=>rejectedWrites++));
        Reject(()=>MapTalkVoices.Export([new("c8900",[37000000])],p=>Read(sourceRoot,p),p=>p=="sound/sprj_c8900.itl"?new byte[0]:Read(nativeRoot,p),(p,b)=>rejectedWrites++));
        foreach(string suffix in new[]{"","_eng","_ded","_esa","_ese","_frf","_iti","_jaj","_ptb"})
            Reject(()=>MapTalkVoices.Export([new("c8900",[37000000])],p=>Read(sourceRoot,p),p=>p=="sound/sprj_c8900"+suffix+".fsb"?new byte[0]:Read(nativeRoot,p),(p,b)=>rejectedWrites++));
        Reject(()=>MapTalkVoices.Export([new("c8900",[37000000]),new("c8901",[37000000])],p=>Read(sourceRoot,p),p=>Read(nativeRoot,p),(p,b)=>rejectedWrites++));
        Check(hashes.All(h=>SHA256.HashData(File.ReadAllBytes(h.Key)).SequenceEqual(h.Value)),"Original source/native asset changed");
        string report="out/map-editor/quests-investigation/talk-audit/map-talk-tests.json";
        Directory.CreateDirectory(Path.GetDirectoryName(report)!);File.WriteAllText(report,JsonSerializer.Serialize(new{Results=results,OriginalFiles=hashes.Count,NativeNameAllocationFixtures=nativeNameFixtures,Flags=flags,GameplayValidated=false},new JsonSerializerOptions{WriteIndented=true}));
        Console.WriteLine($"PASS: three bounded source NPC conversations, 45 rows, both native target maps, {nativeNameFixtures} native name-allocation fixtures, quest branches/interruption and {hashes.Count} unchanged original files; gameplay remains unverified.");
    }
}
