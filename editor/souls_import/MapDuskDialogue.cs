// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Security.Cryptography;

// Only the first rescue offer. Source spell stock, summon signs and the later
// DLC/prisoner conversation require their own working services and map state.
internal static class MapDuskDialogue {
    internal const int Talk=404970,Prompt=900091900,FirstRow=900091000;
    internal sealed record Speech(string Name,int SourceState,int SourceRow,int NativeRow,int Lines);
    internal sealed record Result(IReadOnlyList<Speech> Speeches,MapTalkVoices.Result Voices,int Languages,string SourceVoiceBank);
    static readonly (string Name,int State,int Row)[] Selection=[("introduction",24,15000000),("repeat offer",34,15000020),("acceptance",40,15000050),("refusal",41,15000100)];
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Dusk dialogue: "+why);}
    static int Number(PARAM.Row row,string field)=>Convert.ToInt32(row[field].Value);
    static int Constant(byte[] b)=>b.Length==2&&b[0]<=127&&b[1]==0xa1?b[0]-64:b.Length==6&&b[0]==0x82&&b[5]==0xa1?BitConverter.ToInt32(b,1):throw new InvalidDataException("Dusk ESD constant changed");
    static string? Language(string name)=>name.ToLowerInvariant() switch {"engus" or "enggb"=>"ENGLISH","deude"=>"GERMAN","frafr"=>"FRENCH","itait"=>"ITALIAN","jpnjp" or "japanese"=>"JAPANESE","korkr"=>"KOREAN","polpl"=>"POLISH","rusru"=>"RUSSIAN","spaes"=>"SPANISH","spaar"=>"NSPANISH","porpt" or "porbr"=>"PORTUGUESE","zhocn"=>"SCHINESE","zhotw"=>"TCHINESE",_=>null};
    internal static Result Export(Func<string,byte[]?> source,Func<string,byte[]> native,Func<string,byte[]?> composed,
        string nativeRoot,string definitions,Dictionary<string,byte[]> files) {
        byte[] ReadSource(string path)=>source(path)??throw new FileNotFoundException("Source Dusk dialogue resource unavailable",path);
        byte[] Read(string path)=>files.TryGetValue(path,out var bytes)?bytes:composed(path)??native(path);
        var sourceTalk=BND3.Read(ReadSource("script/talk/m12_00_00_00.talkesdbnd.dcx"));
        var sourceFile=sourceTalk.Files.Single(f=>Leaf(f.Name)=="t120605.esd");
        Need(Convert.ToHexStringLower(SHA256.HashData(sourceFile.Bytes.Span))=="bdf7f44106c4acef910f03e949044f1a67c266aeac61f6b61ade815216a61c14","source t120605 graph changed");
        var states=ESD.Read(sourceFile.Bytes).StateGroups[1];
        foreach(var b in Selection)Need(states[b.State].EntryCommands.Any(c=>c.CommandBank==1&&c.CommandID==1&&Constant(c.Arguments[0])==b.Row),"source speech changed");
        foreach(var pair in new[]{(25,71200000),(37,71200001),(38,71200002),(54,11200590),(55,11200591)})
            Need(states[pair.Item1].EntryCommands.Any(c=>c.CommandBank==1&&c.CommandID==11&&c.Arguments.Select(Constant).SequenceEqual(new[]{pair.Item2,1})),"source completion flag changed");
        Need(states[39].EntryCommands.Any(c=>c.CommandBank==1&&c.CommandID==17&&c.Arguments.Select(Constant).SequenceEqual(new[]{8,10020040,3,4,1})),"source choice changed");
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
        string prompt=SourceText("ENGLISH",30)?.GetValueOrDefault(10020040)??throw new InvalidDataException("Source choice text unavailable");
        var locales=new List<(string Path,string? Language,BND4 Binder,BinderFile Speech,FMG Text,BinderFile Dialog,FMG Choices)>();
        foreach(string dir in Directory.EnumerateDirectories(Path.Combine(nativeRoot,"msg")).Order(StringComparer.Ordinal)) {
            string language=Path.GetFileName(dir),path=$"msg/{language}/menu.msgbnd.dcx";if(!File.Exists(Path.Combine(nativeRoot,path)))continue;
            var bank=BND4.Read(Read(path));var text=bank.Files.Single(f=>f.ID==1);var dialog=bank.Files.Single(f=>f.ID==30);var choices=FMG.Read(dialog.Bytes);
            Need(!choices.Entries.Any(e=>e.ID==Prompt),"owned choice message occupied");string? lang=Language(language);
            var local=lang==null?null:SourceText(lang,30)?.GetValueOrDefault(10020040);choices.Entries.Add(new(choices,Prompt,string.IsNullOrWhiteSpace(local)?prompt:local));
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
                var row=new PARAM.Row(target,"Imported Dusk rescue: "+b.Name+" ["+id+"]",definition);foreach(var c in row.Cells)c.Value=-1;row["msgId"].Value=target;row["voiceId"].Value=-1;row["returnPos"].Value=0;rows.Rows.Add(row);voiceRows.Add((row,Number(fromRow,"voiceId")));
                foreach(var locale in locales){var value=locale.Language==null?null:SourceText(locale.Language,1)?.GetValueOrDefault(msg);locale.Text.Entries.Add(new(locale.Text,target,string.IsNullOrWhiteSpace(value)?fallback:value));}count++;
            }
            Need(count is >0 and <99,"source dialogue sequence is empty/unbounded");speeches.Add(new(b.Name,b.State,b.Row,first,count));
        }
        string talkPath="script/talk/m40_04_00_00.talkesdbnd.dcx";var talkBank=BND4.Read(Read(talkPath));
        Need(!talkBank.Files.Any(f=>Leaf(f.Name)=="t"+Talk+".esd"),"owned talk actor occupied");
        var esd=Build(donor,speeches);var encoded=esd.Write();var check=ESD.Read(encoded);Need(check.LongFormat&&check.DarkSoulsCount==2&&check.StateGroups.Count==1,"native ESD readback");
        var carrier=talkBank.Files[0];string prefix=carrier.Name[..(carrier.Name.LastIndexOfAny(['\\','/'])+1)];
        talkBank.Files.Add(new(carrier.Flags,talkBank.Files.Max(f=>f.ID)+1,prefix+"t"+Talk+".esd",encoded));
        byte[]? Optional(string path)=>files.TryGetValue(path,out var bytes)?bytes:composed(path)??(File.Exists(Path.Combine(nativeRoot,path))?native(path):null);
        const string voiceBank="sound/frpg_sm12.fsb";
        // The shared PCM/project writer's initial caller was Firelink. This
        // encounter selects its actual map12 bank explicitly, never substitutes
        // a line merely because another map has a similar voice identifier.
        byte[]? VoiceSource(string path)=>source(path=="sound/frpg_sm10.fsb"?voiceBank:path);
        var voices=MapTalkVoices.Export([new("c8910",voiceRows.Select(r=>r.Voice).ToArray())],VoiceSource,Optional,(path,bytes)=>files[path]=bytes,firstVoice:249974000);
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
        foreach(int i in new[]{0,1,2,3,4,10,11,12,13,20,21,22,23,30,31,32})states.Add(i,new());
        void Go(int from,int to,byte[] condition)=>states[from].Conditions.Add(new(to,E(condition)));
        byte[] Flag(int source,int value)=>Eq(F(15,V(MapNpcQuestState.Flag(source))),V(value));
        var abort=Any(Any(Eq(F(3),V(1)),Eq(F(19),V(1))),[..F(1),..V(5),0x94]);
        Go(0,1,V(1));Go(1,2,All(Flag(1121,1),F(56,V(6000))));
        Go(2,20,Flag(71200001,1));Go(2,22,Flag(71200002,1));Go(2,12,Flag(71200000,1));Go(2,10,V(1));
        foreach(var (name,speech,done) in new[]{("introduction",10,11),("repeat offer",12,13),("acceptance",20,21),("refusal",22,23)}) {
            states[speech].EntryCommands.Add(C(1,speeches.Single(b=>b.Name==name).NativeRow,-1,-1));Go(speech,3,abort);Go(speech,done,Eq(F(57,V(0)),V(1)));
        }
        states[11].EntryCommands.Add(C(11,MapNpcQuestState.Flag(71200000),1));Go(11,30,V(1));Go(13,30,V(1));
        states[30].EntryCommands.Add(C(17,2,Prompt,3,4,2));Go(30,3,abort);Go(30,31,Eq(F(22),V(1)));Go(30,32,Eq(F(21),V(0)));
        states[31].EntryCommands.Add(C(11,MapNpcQuestState.Flag(71200001),1));Go(31,20,V(1));
        states[32].EntryCommands.Add(C(11,MapNpcQuestState.Flag(71200002),1));Go(32,22,V(1));
        states[21].EntryCommands.Add(C(11,MapNpcQuestState.Flag(11200590),1));Go(21,3,V(1));
        states[23].EntryCommands.Add(C(11,MapNpcQuestState.Flag(11200591),1));Go(23,3,V(1));
        foreach(var c in new[]{C(71),C(8,0),C(9),C(36)})states[3].EntryCommands.Add(c);Go(3,4,V(1));Go(4,1,Eq(F(56,V(6000)),V(0)));return esd;
    }
}
