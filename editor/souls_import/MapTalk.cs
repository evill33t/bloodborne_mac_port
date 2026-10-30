// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;

internal static class MapTalk {
    internal sealed record Branch(string Name,int SourceState,int SourceTalkParam,int NativeTalkParam,int Lines,
        IReadOnlyList<int> SourceCompletionFlags,IReadOnlyList<int> NativeCompletionFlags,string Adaptation);
    internal sealed record Actor(string Name,int SourceEntity,int NativeEntity,int SourceTalkId,int NativeTalkId,
        IReadOnlyList<Branch> Branches);
    internal sealed record Result(IReadOnlyList<Actor> Actors,int TalkParamRows,int Languages,MapTalkVoices.Result Voices,IReadOnlyList<string> Limitations);
    sealed record Selection(string Name,int State,int Row,int Completion,int[] Flags,int MaxLines=99,string Adaptation="");
    sealed record Plan(string Name,int Entity,int Talk,Selection[] Selections);

    // These are bounded source conversations, not a transplanted native NPC quest.
    static readonly Plan[] Plans=[
        new("Crestfallen Warrior",6270,102627,[
            new("introduction",27,37000000,28,[71100026,11026105]),
            new("bell directions",31,37000100,32,[71100027,11026105]),
            new("questions exhausted",33,37000200,34,[71100028,11026105]),
            new("upper bell",65,37000400,66,[71100046,71100026,71100027,71100028]),
            new("lower bell",78,37000450,66,[71100046,71100026,71100027,71100028]),
            new("both bells",49,37002200,50,[71100049,71100046,71100026,71100027,71100028,71100047,71100048]),
            new("repeat",69,37002500,70,[71100053,11026105])]),
        new("Petrus of Thorolund",6080,102608,[
            new("introduction",101,18000000,102,[71300065]),
            new("warm greeting",103,18000100,104,[71300066],3,"Stops before the unported copper-coin gift; its reward flag is not written."),
            new("repeat",88,18011400,-1,[],1,"Retains only the greeting; miracle teaching and covenant menus are not exposed.")]),
        new("Anastacia of Astora",6060,102606,[
            new("silent greeting",31,16000000,-1,[],1,"Binds source talk proxy c1000_0001 to visible actor6060. The source silent baseline is retained; Estus upgrades and resurrection dialogue are not exposed.")])
    ];

    internal static Result Export(MSBB map,string profile,string sourceMap,string nativeRoot,string definitionsDirectory,
        Func<string,byte[]?> readSource,Func<int,int> mapFlag,IReadOnlyDictionary<int,int> actorEntities,
        Action<string,byte[]> write,Func<string,byte[]?>? readNative=null,string nativeMap="m24_01_00_00") {
        if(profile is not ("DS1" or "DS1R")||sourceMap!="m10_02_00_00")
            throw new NotSupportedException("Bounded source dialogue is implemented only for DS1 Firelink Shrine");
        byte[] Source(string path)=>readSource(path)??throw new FileNotFoundException("Source dialogue asset unavailable: "+path);
        byte[]? NativeOptional(string path)=>readNative?.Invoke(path)??
            (File.Exists(Path.Combine(nativeRoot,path))?File.ReadAllBytes(Path.Combine(nativeRoot,path)):null);
        byte[] Native(string path)=>NativeOptional(path)??throw new FileNotFoundException("Native dialogue asset unavailable",path);
        static string Leaf(string path)=>Path.GetFileName(path.Replace('\\','/'));
        var sourceTalk=BND3.Read(Source("script/talk/m10_02_00_00.talkesdbnd.dcx"));
        var sourceGame=BND3.Read(Source("param/GameParam/GameParam.parambnd.dcx"));
        var sourceRows=PARAM.Read(sourceGame.Files.Single(f=>Leaf(f.Name)=="TalkParam.param").Bytes);
        sourceRows.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitionsDirectory,"TalkParam.xml")));
        var game=BND4.Read(Native("param/gameparam/gameparam.parambnd.dcx"));
        var talkFile=game.Files.Single(f=>Leaf(f.Name)=="TalkParam.param");
        var nativeDef=PARAMDEF.XmlDeserialize(Path.GetFullPath(Path.Combine(definitionsDirectory,"../../BB/Defs/TalkParam.xml")));
        var rows=PARAM.Read(talkFile.Bytes);rows.ApplyParamdef(nativeDef);
        if(rows.ParamType!="TALK_PARAM_ST"||rows.DetectedSize!=68||nativeDef.GetRowSize()!=68||sourceRows.DetectedSize!=32||
            rows.BigEndian||rows.Format2D!=PARAM.FormatFlags1.LongDataOffset)
            throw new InvalidDataException("Unexpected source/native TalkParam layout");
        if(nativeMap.Length!=12||nativeMap[0]!='m'||!int.TryParse(nativeMap.AsSpan(1,2),out var area)||
            !int.TryParse(nativeMap.AsSpan(4,2),out var block)||block>9)
            throw new ArgumentException("Expected a native map with a single-digit talk block",nameof(nativeMap));
        var talkPath=$"script/talk/{nativeMap}.talkesdbnd.dcx";
        var talk=BND4.Read(Native(talkPath));
        var nativeTemplate=ESD.Read(talk.Files[0].Bytes);
        if(!nativeTemplate.LongFormat||nativeTemplate.DarkSoulsCount!=2)throw new InvalidDataException("Expected native Bloodborne talk format");
        var locales=new List<(string Language,BND4 Binder,BinderFile File,FMG Text)>();
        foreach(var directory in Directory.EnumerateDirectories(Path.Combine(nativeRoot,"msg")).Order(StringComparer.Ordinal)) {
            var language=Path.GetFileName(directory);var relative=$"msg/{language}/menu.msgbnd.dcx";
            if(!File.Exists(Path.Combine(nativeRoot,relative)))continue;
            var binder=BND4.Read(Native(relative));var file=binder.Files.Single(f=>f.ID==1);
            locales.Add((language,binder,file,FMG.Read(file.Bytes)));
        }
        if(locales.Count==0)throw new InvalidDataException("Native conversation text is unavailable");
        var sourceLanguages=new Dictionary<string,Dictionary<int,string?>>(StringComparer.Ordinal);
        Dictionary<int,string?>? Text(string language) {
            if(sourceLanguages.TryGetValue(language,out var found))return found;
            var bytes=readSource($"msg/{language}/menu.msgbnd.dcx")??readSource($"msg/{language}/menu.msgbnd");
            if(bytes==null)return null;
            var fmg=FMG.Read(BND3.Read(bytes).Files.Single(f=>f.ID==1).Bytes);
            var result=fmg.Entries.ToDictionary(e=>e.ID,e=>(string?)e.Text);sourceLanguages.Add(language,result);return result;
        }
        var english=Text("ENGLISH")??throw new InvalidDataException("Source English dialogue is unavailable");
        var used=rows.Rows.Select(r=>r.ID).Concat(locales.SelectMany(l=>l.Text.Entries.Select(e=>e.ID))).ToHashSet();
        int nextRow=900070000,nextTalk=area*10000+block*1000+970,lastTalk=area*10000+block*1000+999,
            nextFile=talk.Files.Max(f=>f.ID)+1,totalRows=0;
        var actors=new List<Actor>();var voiceRows=new List<(PARAM.Row Row,int SourceVoice,string Model)>();
        foreach(var plan in Plans) {
            if(!actorEntities.TryGetValue(plan.Entity,out var entity))throw new InvalidDataException("Imported dialogue actor has no target mapping: "+plan.Entity);
            var actor=map.Parts.Enemies.Single(p=>p.EntityID==entity);
            var source=ESD.Read(sourceTalk.Files.Single(f=>Leaf(f.Name)==$"t{plan.Talk}.esd").Bytes);
            if(source.DarkSoulsCount!=1||!source.StateGroups.TryGetValue(1,out var sourceStates))throw new InvalidDataException("Unexpected source dialogue graph");
            var branches=new List<Branch>();
            foreach(var selection in plan.Selections) {
                if(!sourceStates.TryGetValue(selection.State,out var state)||!state.EntryCommands.Any(c=>c.CommandBank==1&&c.CommandID==1&&c.Arguments.Count==3&&Constant(c.Arguments[0])==selection.Row))
                    throw new InvalidDataException("Source dialogue branch changed: "+plan.Name+" / "+selection.Name);
                if(selection.Completion>=0) {
                    var completion=sourceStates[selection.Completion];
                    foreach(var flag in selection.Flags)
                        if(!completion.EntryCommands.Any(c=>c.CommandBank==1&&c.CommandID==11&&c.Arguments.Count==2&&Constant(c.Arguments[0])==flag&&Constant(c.Arguments[1])==1))
                            throw new InvalidDataException("Source dialogue completion changed: "+flag);
                }
                var sequence=new List<PARAM.Row>();
                for(int id=selection.Row;id<selection.Row+selection.MaxLines;++id) {
                    var row=sourceRows.Rows.SingleOrDefault(r=>r.ID==id);if(row==null)break;
                    if(Number(row,"returnPos")!=0||Number(row,"eventId")!=-1||Number(row,"reactionId")!=-1)
                        throw new InvalidDataException("Source dialogue requires an unsupported TalkParam continuation: "+id);
                    sequence.Add(row);
                }
                if(sequence.Count==0||sequence.Count>=99)throw new InvalidDataException("Source dialogue sequence is empty or unbounded");
                while(Enumerable.Range(nextRow,100).Any(used.Contains))nextRow=checked(nextRow+100);
                var targetRoot=nextRow;nextRow=checked(nextRow+100);
                for(int i=0;i<sequence.Count;++i) {
                    var sourceRow=sequence[i];var msg=Number(sourceRow,"msgId");
                    if(!english.TryGetValue(msg,out var fallback)||string.IsNullOrWhiteSpace(fallback))throw new InvalidDataException("Source dialogue text is missing: "+msg);
                    int id=targetRoot+i;used.Add(id);var row=new PARAM.Row(id,$"Imported {plan.Name}: {selection.Name} [{sourceRow.ID}]",nativeDef);
                    row["msgId"].Value=id;row["voiceId"].Value=-1;row["returnPos"].Value=0;
                    // Source animation, effects, reactions and event IDs are game-specific.
                    foreach(var cell in row.Cells.Where(c=>c.Def.InternalName!="msgId"&&c.Def.InternalName!="returnPos"))cell.Value=-1;
                    rows.Rows.Add(row);voiceRows.Add((row,Number(sourceRow,"voiceId"),actor.ModelName));++totalRows;
                    foreach(var locale in locales) {
                        var language=SourceLanguage(locale.Language);var local=language==null?null:Text(language);
                        var line=local?.GetValueOrDefault(msg);locale.Text.Entries.Add(new FMG.Entry(locale.Text,id,string.IsNullOrWhiteSpace(line)?fallback:line));
                    }
                }
                var flags=selection.Flags.Select(mapFlag).ToArray();
                if(flags.Any(f=>f<=0)||flags.Distinct().Count()!=flags.Length)throw new InvalidDataException("Dialogue flag mapping is invalid");
                branches.Add(new Branch(selection.Name,selection.State,selection.Row,targetRoot,sequence.Count,selection.Flags,flags,selection.Adaptation));
            }
            while(talk.Files.Any(f=>Leaf(f.Name)==$"t{nextTalk}.esd")||map.Parts.Enemies.Any(p=>p.TalkID==nextTalk))++nextTalk;
            if(nextTalk>lastTalk)throw new InvalidDataException("No unused imported talk ID remains in the target map");
            var esd=Build(plan.Entity,branches,mapFlag,nativeTemplate);esd.Name=$"t{nextTalk}";
            var encoded=esd.Write();var check=ESD.Read(encoded);
            if(!check.LongFormat||check.DarkSoulsCount!=2||check.StateGroups.Count!=1||check.Name!=$"t{nextTalk}")throw new InvalidDataException("Imported dialogue ESD readback failed");
            var donor=talk.Files[0];var slash=donor.Name.Replace('\\','/').LastIndexOf('/');var prefix=slash<0?"":donor.Name[..(slash+1)];
            talk.Files.Add(new BinderFile(donor.Flags,nextFile++,prefix+$"t{nextTalk}.esd",encoded));
            actor.TalkID=nextTalk;actors.Add(new Actor(plan.Name,plan.Entity,entity,plan.Talk,nextTalk,branches));++nextTalk;
        }
        if(area!=24)throw new NotSupportedException("Imported Firelink voice registration currently targets native area24");
        var voiceOwners=actors.Select(a=>map.Parts.Enemies.Single(p=>p.EntityID==a.NativeEntity).ModelName)
            .Select(model=>new MapTalkVoices.Owner(model,voiceRows.Where(r=>r.Model==model).Select(r=>r.SourceVoice).ToArray())).ToArray();
        var voices=MapTalkVoices.Export(voiceOwners,readSource,NativeOptional,write);
        var voiceIds=voices.Clips.ToDictionary(c=>c.SourceVoice,c=>c.NativeVoice);
        foreach(var binding in voiceRows)binding.Row["voiceId"].Value=voiceIds[binding.SourceVoice];
        talkFile.Bytes=WriteRows(rows);
        write("param/gameparam/gameparam.parambnd.dcx",game.Write(game.Compression));
        write(talkPath,talk.Write(talk.Compression));
        foreach(var locale in locales) {locale.File.Bytes=locale.Text.Write();write($"msg/{locale.Language}/menu.msgbnd.dcx",locale.Binder.Write(locale.Binder.Compression));}
        return new Result(actors,totalRows,locales.Count,voices,[
            "Bounded source dialogue only; complete source NPC quests, hostility, shops, covenants, Estus upgrades, rewards and departures are not translated.",
            "Selected original source speech is converted into per-actor owned native sound projects. Source talk animations are not converted.",
            "Native ESD/text/parameter/audio readback does not establish live interaction or quest persistence; native gameplay validation is required."
        ]);
    }

    internal static ESD Build(int sourceEntity,IReadOnlyList<Branch> branches,Func<int,int> mapFlag,ESD template) {
        var esd=new ESD(true,2){Name="Imported Firelink dialogue",Unk70=template.Unk70,Unk74=template.Unk74,Unk78=template.Unk78,Unk7C=template.Unk7C};
        var states=new Dictionary<long,ESD.State>();esd.StateGroups.Add(1,states);
        ESD.State State(long id){var s=new ESD.State();states.Add(id,s);return s;}
        void Go(ESD.State state,long target,byte[] expression)=>state.Conditions.Add(new ESD.Condition(target,End(expression)));
        byte[] Flag(int source,int value)=>Binary(0x95,Call(15,Value(mapFlag(source))),Value(value));
        var initial=State(0);Go(initial,1,Value(1));
        var idle=State(1);Go(idle,2,Call(56,Value(6000)));
        var dispatch=State(2);var abort=State(3);
        abort.EntryCommands.Add(Command(71));abort.EntryCommands.Add(Command(8,0));abort.EntryCommands.Add(Command(9));abort.EntryCommands.Add(Command(36));Go(abort,4,Value(1));
        var rearm=State(4);Go(rearm,1,Binary(0x95,Call(56,Value(6000)),Value(0)));
        var targets=new Dictionary<string,long>(StringComparer.Ordinal);
        for(int i=0;i<branches.Count;++i) {
            var branch=branches[i];long speech=100+i*2,complete=speech+1;targets.Add(branch.Name,speech);
            var speaking=State(speech);speaking.EntryCommands.Add(Command(1,branch.NativeTalkParam,-1,-1));
            Go(speaking,3,Any(Binary(0x95,Call(3),Value(1)),Binary(0x95,Call(19),Value(1)),Binary(0x94,Call(1),Value(5))));
            Go(speaking,complete,Binary(0x95,Call(57,Value(0)),Value(1)));
            var done=State(complete);foreach(var flag in branch.NativeCompletionFlags)done.EntryCommands.Add(Command(11,flag,1));Go(done,3,Value(1));
        }
        if(sourceEntity==6270) {
            var both=All(Flag(11010700,1),Flag(11400200,1));
            Go(dispatch,targets["both bells"],All(both,Flag(71100049,0)));
            Go(dispatch,targets["repeat"],both);
            Go(dispatch,targets["lower bell"],All(Flag(11400200,1),Flag(71100046,0)));
            Go(dispatch,targets["upper bell"],All(Flag(11010700,1),Flag(71100046,0)));
            Go(dispatch,targets["introduction"],Flag(71100026,0));
            Go(dispatch,targets["bell directions"],Flag(71100027,0));
            Go(dispatch,targets["questions exhausted"],Flag(71100028,0));
            Go(dispatch,targets["repeat"],Value(1));
        } else if(sourceEntity==6080) {
            Go(dispatch,targets["introduction"],Flag(71300065,0));
            Go(dispatch,targets["warm greeting"],Flag(71300066,0));Go(dispatch,targets["repeat"],Value(1));
        } else if(sourceEntity==6060)Go(dispatch,targets["silent greeting"],Value(1));
        else throw new NotSupportedException("No bounded source dialogue dispatcher for "+sourceEntity);
        return esd;
    }

    static int Number(PARAM.Row row,string name)=>Convert.ToInt32(row[name].Value);
    // SoulsFormats emits an empty string for a null row name. Native long row
    // headers use a zero name offset; retain those original opaque entries.
    internal static byte[] WriteRows(PARAM rows) {
        if(rows.BigEndian||rows.Format2D!=PARAM.FormatFlags1.LongDataOffset)
            throw new InvalidDataException("Expected native 64-bit parameter row headers");
        var nulls=rows.Rows.Select((row,index)=>(row.Name,index)).Where(p=>p.Name==null).Select(p=>p.index).ToArray();
        var bytes=rows.Write();foreach(int index in nulls)Array.Clear(bytes,64+index*24+16,8);return bytes;
    }
    static int Constant(byte[] bytes) {
        if(bytes.Length==2&&bytes[0]<=0x7f&&bytes[1]==0xa1)return bytes[0]-64;
        if(bytes.Length==6&&bytes[0]==0x82&&bytes[5]==0xa1)return BitConverter.ToInt32(bytes,1);
        throw new InvalidDataException("Expected a constant source ESD argument");
    }
    internal static byte[] Value(int value)=>value is >=-64 and <=63?[(byte)(value+64)]:[0x82,..BitConverter.GetBytes(value)];
    static byte[] End(byte[] value)=>[..value,0xa1];
    // Retain the shipped ContinueIfFalse marker. Its handler is a no-op in the
    // inspected BB executable; it does not replace boolean &&/|| operators.
    static byte[] Call(int function,params byte[][] args)=>[..Value(function),..args.SelectMany(a=>a),(byte)(0x84+args.Length),0xa6];
    static byte[] Binary(byte op,byte[] a,byte[] b)=>[..a,..b,op];
    static byte[] All(params byte[][] args)=>args.Aggregate((a,b)=>Binary(0x98,a,b));
    static byte[] Any(params byte[][] args)=>args.Aggregate((a,b)=>Binary(0x99,a,b));
    static ESD.CommandCall Command(int id,params int[] args)=>new(1,id,args.Select(a=>End(Value(a))).ToArray());
    static string? SourceLanguage(string language)=>language.ToLowerInvariant() switch {
        "engus" or "enggb"=>"ENGLISH","deude"=>"GERMAN","frafr"=>"FRENCH","itait"=>"ITALIAN",
        "jpnjp" or "japanese"=>"JAPANESE","korkr"=>"KOREAN","polpl"=>"POLISH","rusru"=>"RUSSIAN",
        "spaes"=>"SPANISH","spaar"=>"NSPANISH","porpt" or "porbr"=>"PORTUGUESE","zhocn"=>"SCHINESE","zhotw"=>"TCHINESE",_=>null
    };
}
