// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Security.Cryptography;

// Original non-Way-of-White introduction and ordinary repeat. Hunters have no
// imported covenant membership; pardon, covenant and shop services stay absent.
internal static class MapOswaldDialogue {
    internal sealed record Speech(string Name,int SourceState,int SourceRow,int NativeRow,int Lines);
    internal sealed record Result(int Talk,IReadOnlyList<Speech> Speeches,MapTalkVoices.Result Voices,int Languages,string SourceESDSha256);
    static readonly (string Name,int State,int Row)[] Selection=[("nonmember introduction",59,47001400),("ordinary repeat",14,47000100)];
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Oswald dialogue: "+why);}
    static int N(PARAM.Row row,string field)=>Convert.ToInt32(row[field].Value);
    static int Constant(byte[] b)=>b.Length==2&&b[0]<=127&&b[1]==0xa1?b[0]-64:b.Length==6&&b[0]==0x82&&b[5]==0xa1?BitConverter.ToInt32(b,1):throw new InvalidDataException("Oswald ESD constant changed");
    static string? Language(string name)=>name.ToLowerInvariant() switch {"engus" or "enggb"=>"ENGLISH","deude"=>"GERMAN","frafr"=>"FRENCH","itait"=>"ITALIAN","jpnjp" or "japanese"=>"JAPANESE","korkr"=>"KOREAN","polpl"=>"POLISH","rusru"=>"RUSSIAN","spaes"=>"SPANISH","spaar"=>"NSPANISH","porpt" or "porbr"=>"PORTUGUESE","zhocn"=>"SCHINESE","zhotw"=>"TCHINESE",_=>null};
    internal static Result Export(Func<string,byte[]?> source,Func<string,byte[]> native,Func<string,byte[]?> composed,
        string nativeRoot,string definitions,Dictionary<string,byte[]> files) {
        byte[] ReadSource(string path)=>source(path)??throw new FileNotFoundException("Oswald source dialogue unavailable",path);
        byte[] Read(string path)=>files.TryGetValue(path,out var bytes)?bytes:composed(path)??native(path);
        const string sourceMap="m10_01_00_00",targetMap="m40_03_00_00",model="c8914";
        const int sourceID=101637,talk=403981,firstRow=900097000;var selections=Selection;
        var file=BND3.Read(ReadSource("script/talk/"+sourceMap+".talkesdbnd.dcx")).Files.Single(f=>Leaf(f.Name)=="t"+sourceID+".esd");
        string hash=Convert.ToHexStringLower(SHA256.HashData(file.Bytes.Span));
        Need(hash=="3bf9a946e2d8e3c93aeb47ef63504edadde0b8b90b4477a77a9ef68b0fd066a0","source graph changed");
        var sourceStates=ESD.Read(file.Bytes).StateGroups[1];
        foreach(var b in selections)Need(sourceStates[b.State].EntryCommands.Any(c=>c.CommandBank==1&&c.CommandID==1&&Constant(c.Arguments[0])==b.Row),"source speech changed");
        foreach(var pair in new[]{(60,71800057),(60,11016111),(15,71800058),(15,11016111)})
            Need(sourceStates[pair.Item1].EntryCommands.Any(c=>c.CommandBank==1&&c.CommandID==11&&c.Arguments.Select(Constant).SequenceEqual(new[]{pair.Item2,1})),"source speech completion changed");
        var from=BND3.Read(ReadSource("param/GameParam/GameParam.parambnd.dcx"));var sourceRows=PARAM.Read(from.Files.Single(f=>Leaf(f.Name)=="TalkParam.param").Bytes);
        sourceRows.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitions,"TalkParam.xml")));
        var game=BND4.Read(Read("param/gameparam/gameparam.parambnd.dcx"));var talkParam=game.Files.Single(f=>Leaf(f.Name)=="TalkParam.param");var rows=PARAM.Read(talkParam.Bytes);
        var definition=PARAMDEF.XmlDeserialize(Path.GetFullPath(Path.Combine(definitions,"../../BB/Defs/TalkParam.xml")));rows.ApplyParamdef(definition);
        Need(sourceRows.DetectedSize==32&&rows.DetectedSize==68&&rows.Format2D==PARAM.FormatFlags1.LongDataOffset,"TalkParam ABI changed");
        var texts=new Dictionary<string,Dictionary<int,string?>>(StringComparer.Ordinal);
        Dictionary<int,string?>? Text(string language) {
            if(texts.TryGetValue(language,out var old))return old;
            var bytes=source("msg/"+language+"/menu.msgbnd.dcx")??source("msg/"+language+"/menu.msgbnd");if(bytes==null)return null;
            var result=FMG.Read(BND3.Read(bytes).Files.Single(f=>f.ID==1).Bytes).Entries.ToDictionary(e=>e.ID,e=>(string?)e.Text);texts.Add(language,result);return result;
        }
        var english=Text("ENGLISH")??throw new InvalidDataException("Source English subtitles unavailable");
        var locales=new List<(string Path,string? Language,BND4 Binder,BinderFile File,FMG Text)>();
        foreach(string dir in Directory.EnumerateDirectories(Path.Combine(nativeRoot,"msg")).Order(StringComparer.Ordinal)) {
            string language=Path.GetFileName(dir),path="msg/"+language+"/menu.msgbnd.dcx";if(!File.Exists(Path.Combine(nativeRoot,path)))continue;
            var bank=BND4.Read(Read(path));var entry=bank.Files.Single(f=>f.ID==1);locales.Add((path,Language(language),bank,entry,FMG.Read(entry.Bytes)));
        }
        Need(locales.Count>0,"native subtitle locales unavailable");var speeches=new List<Speech>();var voiceRows=new List<(PARAM.Row Row,int Voice)>();
        for(int index=0;index<selections.Length;index++) {
            var b=selections[index];int first=firstRow+index*100,count=0;
            for(int id=b.Row;id<b.Row+19;id++) {
                var original=sourceRows.Rows.SingleOrDefault(r=>r.ID==id);if(original==null)break;
                Need(N(original,"returnPos")==0&&N(original,"eventId")==-1&&N(original,"reactionId")==-1,"unported TalkParam continuation");
                int target=first+count,msg=N(original,"msgId");Need(english.TryGetValue(msg,out string? fallback)&&!string.IsNullOrWhiteSpace(fallback),"source subtitle missing");
                Need(!rows.Rows.Any(r=>r.ID==target)&&locales.All(l=>!l.Text.Entries.Any(e=>e.ID==target)),"owned speech reservation occupied");
                var row=new PARAM.Row(target,"Imported Oswald: "+b.Name+" ["+id+"]",definition);foreach(var c in row.Cells)c.Value=-1;
                row["msgId"].Value=target;row["voiceId"].Value=-1;row["returnPos"].Value=0;rows.Rows.Add(row);voiceRows.Add((row,N(original,"voiceId")));
                foreach(var locale in locales){string? value=locale.Language==null?null:Text(locale.Language)?.GetValueOrDefault(msg);locale.Text.Entries.Add(new(locale.Text,target,string.IsNullOrWhiteSpace(value)?fallback:value));}count++;
            }
            Need(count is >0 and <19,"unbounded/empty source sequence");speeches.Add(new(b.Name,b.State,b.Row,first,count));
        }
        var donor=ESD.Read(BND4.Read(native("script/talk/m24_00_00_00.talkesdbnd.dcx")).Files.Single(f=>Leaf(f.Name)=="t240000.esd").Bytes);
        string pathTalk="script/talk/"+targetMap+".talkesdbnd.dcx";var bankTalk=BND4.Read(Read(pathTalk));
        Need(!bankTalk.Files.Any(f=>Leaf(f.Name)=="t"+talk+".esd"),"owned talk actor occupied");
        var esd=Build(donor,speeches);var encoded=esd.Write();var check=ESD.Read(encoded);Need(check.LongFormat&&check.DarkSoulsCount==2&&check.StateGroups.Count==1,"native ESD readback");
        var carrier=bankTalk.Files[0];string prefix=carrier.Name[..(carrier.Name.LastIndexOfAny(['\\','/'])+1)];bankTalk.Files.Add(new(carrier.Flags,bankTalk.Files.Max(f=>f.ID)+1,prefix+"t"+talk+".esd",encoded));
        byte[]? Optional(string path)=>files.TryGetValue(path,out var bytes)?bytes:composed(path)??(File.Exists(Path.Combine(nativeRoot,path))?native(path):null);
        var voices=MapTalkVoices.Export([new(model,voiceRows.Select(r=>r.Voice).ToArray())],source,Optional,(path,bytes)=>files[path]=bytes,firstVoice:249978000);
        var ids=voices.Clips.ToDictionary(c=>c.SourceVoice,c=>c.NativeVoice);foreach(var v in voiceRows)v.Row["voiceId"].Value=ids[v.Voice];
        talkParam.Bytes=MapCharacters.WriteRows(rows);files["param/gameparam/gameparam.parambnd.dcx"]=game.Write(game.Compression);files[pathTalk]=bankTalk.Write(bankTalk.Compression);
        foreach(var locale in locales){locale.File.Bytes=locale.Text.Write();files[locale.Path]=locale.Binder.Write(locale.Binder.Compression);}
        return new(talk,speeches,voices,locales.Count,hash);
    }
    static byte[] V(int n)=>n is >=-64 and <=63?[(byte)(n+64)]:[0x82,..BitConverter.GetBytes(n)];
    static byte[] E(byte[] b)=>[..b,0xa1];static byte[] F(int n,params byte[][] args)=>[..V(n),..args.SelectMany(a=>a),(byte)(0x84+args.Length),0xa6];
    static byte[] Eq(byte[] a,byte[] b)=>[..a,..b,0x95];static byte[] All(byte[] a,byte[] b)=>[..a,..b,0x98];static byte[] Any(byte[] a,byte[] b)=>[..a,..b,0x99];
    static ESD.CommandCall C(int n,params int[] args)=>new(1,n,args.Select(n=>E(V(n))).ToArray());
    internal static ESD Build(ESD template,IReadOnlyList<Speech> speeches) {
        var esd=new ESD(true,2){Name="t403981",Unk70=template.Unk70,Unk74=template.Unk74,Unk78=template.Unk78,Unk7C=template.Unk7C};var states=new Dictionary<long,ESD.State>();esd.StateGroups.Add(1,states);
        foreach(int i in new[]{0,1,2,3,4,20,21,30,31})states.Add(i,new());
        void Go(int a,int b,byte[] expr)=>states[a].Conditions.Add(new(b,E(expr)));
        byte[] Flag(int source,int value)=>Eq(F(15,V(MapNpcQuestState.Flag(source))),V(value));
        var abort=Any(Any(Eq(F(3),V(1)),Eq(F(19),V(1))),[..F(1),..V(5),0x94]);
        void Speak(string name,int state,int done){states[state].EntryCommands.Add(C(1,speeches.Single(s=>s.Name==name).NativeRow,-1,-1));Go(state,3,abort);Go(state,done,Eq(F(57,V(0)),V(1)));}
        void Set(int state,int flag)=>states[state].EntryCommands.Add(C(11,MapNpcQuestState.Flag(flag),1));
        Go(0,1,V(1));Go(1,2,All(All(Flag(1700,1),Flag(11010581,1)),F(56,V(6000))));Go(2,20,Flag(71800057,0));Go(2,30,V(1));
        Speak("nonmember introduction",20,21);Set(21,71800057);Set(21,11016111);Go(21,3,V(1));
        Speak("ordinary repeat",30,31);Set(31,71800058);Set(31,11016111);Go(31,3,V(1));
        foreach(var c in new[]{C(71),C(8,0),C(9),C(36)})states[3].EntryCommands.Add(c);Go(3,4,V(1));Go(4,1,Eq(F(56,V(6000)),V(0)));return esd;
    }
}
