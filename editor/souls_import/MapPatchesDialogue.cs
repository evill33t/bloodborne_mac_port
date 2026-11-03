// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Security.Cryptography;

// The bounded Catacombs encounter. Later Tomb/Firelink/shop/hostility stages
// remain separate source contracts.
internal static class MapPatchesDialogue {
    internal const int Talk=405971,Prompt=900092900,FirstRow=900092000;
    internal sealed record Speech(string Name,int SourceState,int SourceRow,int NativeRow,int Lines);
    internal sealed record Result(IReadOnlyList<Speech> Speeches,MapTalkVoices.Result Voices,int Languages,string SourceVoiceBank);
    static readonly (string Name,int State,int Row)[] Selection=[("introduction",43,42000000),("cleric yes",20,42000020),("cleric no",21,42000040),("first repeat",44,42000100),("late introduction",45,42000200),("late followup",48,42000210),("late repeat",49,42000300),("apology offer",47,42000400),("forgiveness",52,42000500),("forgiveness repeat",50,42000510),("nonmember refusal",54,42000650)];
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Patches dialogue: "+why);}
    static int Number(PARAM.Row row,string field)=>Convert.ToInt32(row[field].Value);
    static int Constant(byte[] b)=>b.Length==2&&b[0]<=127&&b[1]==0xa1?b[0]-64:b.Length==6&&b[0]==0x82&&b[5]==0xa1?BitConverter.ToInt32(b,1):throw new InvalidDataException("Dusk ESD constant changed");
    static string? Language(string name)=>name.ToLowerInvariant() switch {"engus" or "enggb"=>"ENGLISH","deude"=>"GERMAN","frafr"=>"FRENCH","itait"=>"ITALIAN","jpnjp" or "japanese"=>"JAPANESE","korkr"=>"KOREAN","polpl"=>"POLISH","rusru"=>"RUSSIAN","spaes"=>"SPANISH","spaar"=>"NSPANISH","porpt" or "porbr"=>"PORTUGUESE","zhocn"=>"SCHINESE","zhotw"=>"TCHINESE",_=>null};
    internal static Result Export(Func<string,byte[]?> source,Func<string,byte[]> native,Func<string,byte[]?> composed,
        string nativeRoot,string definitions,Dictionary<string,byte[]> files) {
        byte[] ReadSource(string path)=>source(path)??throw new FileNotFoundException("Source Patches dialogue resource unavailable",path);
        byte[] Read(string path)=>files.TryGetValue(path,out var bytes)?bytes:composed(path)??native(path);
        var sourceTalk=BND3.Read(ReadSource("script/talk/m13_00_00_00.talkesdbnd.dcx"));
        var sourceFile=sourceTalk.Files.Single(f=>Leaf(f.Name)=="t130632.esd");
        Need(Convert.ToHexStringLower(SHA256.HashData(sourceFile.Bytes.Span))=="58dea3bb0a44c59c227d8d91833245a166e243c64a1f46f72922add15ba42f05","source t120605 graph changed");
        var states=ESD.Read(sourceFile.Bytes).StateGroups[1];
        foreach(var b in Selection)Need(states[b.State].EntryCommands.Any(c=>c.CommandBank==1&&c.CommandID==1&&Constant(c.Arguments[0])==b.Row),"source speech changed");
        foreach(var pair in new[]{(17,71310066),(18,71310067),(68,71310076),(69,71310077),(46,11300590),(62,71310070),(63,71310071),(65,11300591)})
            Need(states[pair.Item1].EntryCommands.Any(c=>c.CommandBank==1&&c.CommandID==11&&c.Arguments.Select(Constant).SequenceEqual(new[]{pair.Item2,1})),"source completion flag changed");
        Need(states[19].EntryCommands.Any(c=>c.CommandBank==1&&c.CommandID==17&&c.Arguments.Select(Constant).SequenceEqual(new[]{8,10020041,3,4,2})),"source choice changed");
        var donor=ESD.Read(BND4.Read(native("script/talk/m24_00_00_00.talkesdbnd.dcx")).Files.Single(f=>Leaf(f.Name)=="t240000.esd").Bytes);
        Need(donor.LongFormat&&donor.DarkSoulsCount==2&&donor.StateGroups[2147483643][1].EntryCommands.Any(c=>c.CommandBank==1&&c.CommandID==17&&c.Arguments.Select(Constant).SequenceEqual(new[]{2,10010713,3,4,2})),"native confirmation precedent changed");
        var from=BND3.Read(ReadSource("param/GameParam/GameParam.parambnd.dcx"));var sourceRows=PARAM.Read(from.Files.Single(f=>Leaf(f.Name)=="TalkParam.param").Bytes);
        sourceRows.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitions,"TalkParam.xml")));
        var game=BND4.Read(Read("param/gameparam/gameparam.parambnd.dcx"));var talkParam=game.Files.Single(f=>Leaf(f.Name)=="TalkParam.param");var rows=PARAM.Read(talkParam.Bytes);
        var definition=PARAMDEF.XmlDeserialize(Path.GetFullPath(Path.Combine(definitions,"../../BB/Defs/TalkParam.xml")));rows.ApplyParamdef(definition);
        Need(sourceRows.DetectedSize==32&&rows.DetectedSize==68&&rows.Format2D==PARAM.FormatFlags1.LongDataOffset,"source/native TalkParam ABI changed");
        var texts=new Dictionary<(string,int),Dictionary<int,string?>>();
        Dictionary<int,string?>? SourceText(string language,int id) {
            if(texts.TryGetValue((language,id),out var old))return old;
            var bytes=source($"msg/{language}/menu.msgbnd.dcx")??source($"msg/{language}/menu.msgbnd");if(bytes==null)return null;
            var text=FMG.Read(BND3.Read(bytes).Files.Single(f=>f.ID==id).Bytes).Entries.ToDictionary(e=>e.ID,e=>(string?)e.Text);texts.Add((language,id),text);return text;
        }
        var english=SourceText("ENGLISH",1)??throw new InvalidDataException("Source English dialogue unavailable");
        string prompt=SourceText("ENGLISH",30)?.GetValueOrDefault(10020041)??throw new InvalidDataException("Source choice text unavailable");
        var locales=new List<(string Path,string? Language,BND4 Binder,BinderFile Speech,FMG Text,BinderFile Dialog,FMG Choices)>();
        foreach(string dir in Directory.EnumerateDirectories(Path.Combine(nativeRoot,"msg")).Order(StringComparer.Ordinal)) {
            string language=Path.GetFileName(dir),path=$"msg/{language}/menu.msgbnd.dcx";if(!File.Exists(Path.Combine(nativeRoot,path)))continue;
            var bank=BND4.Read(Read(path));var text=bank.Files.Single(f=>f.ID==1);var dialog=bank.Files.Single(f=>f.ID==30);var choices=FMG.Read(dialog.Bytes);
            Need(!choices.Entries.Any(e=>e.ID==Prompt),"owned choice message occupied");string? lang=Language(language);
            var local=lang==null?null:SourceText(lang,30)?.GetValueOrDefault(10020041);choices.Entries.Add(new(choices,Prompt,string.IsNullOrWhiteSpace(local)?prompt:local));
            locales.Add((path,lang,bank,text,FMG.Read(text.Bytes),dialog,choices));
        }
        Need(locales.Count>0,"native subtitle locales unavailable");
        var speeches=new List<Speech>();var voiceRows=new List<(PARAM.Row Row,int Voice)>();
        for(int index=0;index<Selection.Length;index++) {
            var b=Selection[index];int first=FirstRow+index*100,count=0;
            for(int id=b.Row;id<b.Row+99;id++) {
                var fromRow=sourceRows.Rows.SingleOrDefault(r=>r.ID==id);if(fromRow==null)break;
                Need(Number(fromRow,"returnPos")==0&&Number(fromRow,"eventId")==-1&&Number(fromRow,"reactionId")==-1,"unported source TalkParam continuation");
                int target=first+count,msg=Number(fromRow,"msgId");Need(english.TryGetValue(msg,out string? fallback)&&!string.IsNullOrWhiteSpace(fallback),"source speech text unavailable");
                Need(!rows.Rows.Any(r=>r.ID==target)&&locales.All(l=>!l.Text.Entries.Any(e=>e.ID==target)),"owned speech reservation occupied");
                var row=new PARAM.Row(target,"Imported Patches Catacombs: "+b.Name+" ["+id+"]",definition);foreach(var c in row.Cells)c.Value=-1;row["msgId"].Value=target;row["voiceId"].Value=-1;row["returnPos"].Value=0;rows.Rows.Add(row);voiceRows.Add((row,Number(fromRow,"voiceId")));
                foreach(var locale in locales){var value=locale.Language==null?null:SourceText(locale.Language,1)?.GetValueOrDefault(msg);locale.Text.Entries.Add(new(locale.Text,target,string.IsNullOrWhiteSpace(value)?fallback:value));}count++;
            }
            Need(count is >0 and <99,"source dialogue sequence is empty/unbounded");speeches.Add(new(b.Name,b.State,b.Row,first,count));
        }
        string talkPath="script/talk/m40_05_00_00.talkesdbnd.dcx";var talkBank=BND4.Read(Read(talkPath));
        Need(!talkBank.Files.Any(f=>Leaf(f.Name)=="t"+Talk+".esd"),"owned talk actor occupied");
        var esd=Build(donor,speeches);var encoded=esd.Write();var check=ESD.Read(encoded);Need(check.LongFormat&&check.DarkSoulsCount==2&&check.StateGroups.Count==1,"native ESD readback");
        var carrier=talkBank.Files[0];string prefix=carrier.Name[..(carrier.Name.LastIndexOfAny(['\\','/'])+1)];
        talkBank.Files.Add(new(carrier.Flags,talkBank.Files.Max(f=>f.ID)+1,prefix+"t"+Talk+".esd",encoded));
        byte[]? Optional(string path)=>files.TryGetValue(path,out var bytes)?bytes:composed(path)??(File.Exists(Path.Combine(nativeRoot,path))?native(path):null);
        const string voiceBank="sound/frpg_sm13.fsb";
        // The shared PCM/project writer's initial caller was Firelink. This
        // encounter selects its actual map13 bank explicitly, never substitutes
        // a line merely because another map has a similar voice identifier.
        byte[]? VoiceSource(string path)=>source(path=="sound/frpg_sm10.fsb"?voiceBank:path);
        var voices=MapTalkVoices.Export([new("c8911",voiceRows.Select(r=>r.Voice).ToArray())],VoiceSource,Optional,(path,bytes)=>files[path]=bytes,firstVoice:249975000);
        var voiceIds=voices.Clips.ToDictionary(c=>c.SourceVoice,c=>c.NativeVoice);foreach(var binding in voiceRows)binding.Row["voiceId"].Value=voiceIds[binding.Voice];
        talkParam.Bytes=MapCharacters.WriteRows(rows);files["param/gameparam/gameparam.parambnd.dcx"]=game.Write(game.Compression);files[talkPath]=talkBank.Write(talkBank.Compression);
        foreach(var locale in locales){locale.Speech.Bytes=locale.Text.Write();locale.Dialog.Bytes=locale.Choices.Write();files[locale.Path]=locale.Binder.Write(locale.Binder.Compression);}
        return new(speeches,voices,locales.Count,voiceBank);
    }
    static byte[] V(int n)=>n is >=-64 and <=63?[(byte)(n+64)]:[0x82,..BitConverter.GetBytes(n)];
    static byte[] E(byte[] b)=>[..b,0xa1];static byte[] F(int n,params byte[][] args)=>[..V(n),..args.SelectMany(a=>a),(byte)(0x84+args.Length),0xa6];
    static byte[] Eq(byte[] a,byte[] b)=>[..a,..b,0x95];static byte[] Any(byte[] a,byte[] b)=>[..a,..b,0x99];static byte[] All(byte[] a,byte[] b)=>[..a,..b,0x98];
    static ESD.CommandCall C(int n,params int[] args)=>new(1,n,args.Select(n=>E(V(n))).ToArray());
    internal static ESD Build(ESD template,IReadOnlyList<Speech> speeches) {
        var esd=new ESD(true,2){Name="t"+Talk,Unk70=template.Unk70,Unk74=template.Unk74,Unk78=template.Unk78,Unk7C=template.Unk7C};var states=new Dictionary<long,ESD.State>();esd.StateGroups.Add(1,states);
        foreach(int i in new[]{0,6,100,41,42,43,19,17,18,20,21,68,69,44,45,46,48,49,47,64,62,63,52,65,50,54,3,4})states.Add(i,new());
        void Go(int a,int b,byte[] expression)=>states[a].Conditions.Add(new(b,E(expression)));
        byte[] Flag(int source,int value)=>Eq(F(15,V(MapNpcQuestState.Flag(source))),V(value));
        void Set(int state,int source)=>states[state].EntryCommands.Add(C(11,MapNpcQuestState.Flag(source),1));
        var abort=Any(Any(Eq(F(3),V(1)),Eq(F(19),V(1))),[..F(1),..V(5),0x94]);
        Go(0,6,V(1));Go(6,100,All(Any(Flag(1620,1),Flag(1621,1)),F(56,V(6000))));Go(100,41,Flag(1620,1));Go(100,42,Flag(1621,1));Go(100,3,V(1));
        Go(41,43,All(Flag(71310066,0),Flag(71310067,0)));Go(41,21,All(Flag(71310067,1),Flag(71310076,0)));Go(41,20,All(Flag(71310066,1),Flag(71310076,0)));Go(41,44,Flag(1637,1));
        Go(42,50,Flag(11300591,1));Go(42,52,All(Flag(71310070,1),Flag(11300591,0)));Go(42,49,All(Flag(1637,0),Flag(11300590,1)));Go(42,45,Flag(1637,0));Go(42,63,Flag(71310071,1));Go(42,47,V(1));
        void Speak(string name,int state,int done){states[state].EntryCommands.Add(C(1,speeches.Single(s=>s.Name==name).NativeRow,-1,-1));Go(state,3,abort);Go(state,done,Eq(F(57,V(0)),V(1)));}
        Speak("introduction",43,19);Speak("cleric yes",20,68);Speak("cleric no",21,69);Speak("first repeat",44,3);
        Speak("late introduction",45,46);Speak("late followup",48,3);Speak("late repeat",49,3);Speak("apology offer",47,64);
        Speak("forgiveness",52,65);Speak("forgiveness repeat",50,3);Speak("nonmember refusal",54,3);
        foreach(int choice in new[]{19,64}){states[choice].EntryCommands.Add(C(17,2,Prompt,3,4,2));Go(choice,3,abort);Go(choice,choice==19?17:62,Eq(F(22),V(1)));Go(choice,choice==19?18:63,Eq(F(21),V(0)));}
        Set(19,1637);foreach(int flag in new[]{71310066,1638,1637})Set(17,flag);foreach(int flag in new[]{71310067,1637})Set(18,flag);Go(17,20,V(1));Go(18,21,V(1));
        Set(68,71310076);Set(69,71310077);Go(68,3,V(1));Go(69,3,V(1));
        foreach(int flag in new[]{71310068,11300590})Set(46,flag);Go(46,48,V(1));Set(62,71310070);Go(62,52,V(1));Set(63,71310071);Go(63,54,V(1));
        foreach(int flag in new[]{71310072,11300591})Set(65,flag);Go(65,3,V(1));
        foreach(var c in new[]{C(71),C(8,0),C(9),C(36)})states[3].EntryCommands.Add(c);Go(3,4,V(1));Go(4,6,Eq(F(56,V(6000)),V(0)));return esd;
    }
}
