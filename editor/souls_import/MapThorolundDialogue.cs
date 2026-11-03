// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Security.Cryptography;

// Bounded original Firelink party conversations. Source state and completion
// contracts are checked; miracle/covenant services and Tomb rescue are absent.
internal static class MapThorolundDialogue {
    internal sealed record Speech(string Name,int SourceState,int SourceRow,int NativeRow,int Lines,IReadOnlyList<int> CompletionFlags);
    internal sealed record Result(int SourceEntity,int Talk,IReadOnlyList<Speech> Speeches,MapTalkVoices.Result Voices,int Languages,string SourceESDSha256);
    sealed record Selection(string Name,int State,int Row,int Completion,int[] Flags,int Limit=19);
    static Selection[] Select(int id)=>id switch {
        6070=>[new("introduction",27,17000000,28,[71020080]),new("repeat",37,17000100,-1,[])],
        6090=>[new("introduction",14,19000000,15,[71310023]),new("mission",20,19000100,21,[71310024,11026109]),new("prayer",22,19000200,23,[71310025]),new("farewell",17,19000300,-1,[])],
        6100=>[new("greeting",14,20000100,15,[71310032,11026110]),new("repeat",20,20000200,21,[71310033]),new("farewell",17,20000400,-1,[])],
        6080=>[new("introduction",101,18000000,102,[71300065]),new("warm greeting",103,18000100,104,[71300066],3),new("repeat",88,18011400,-1,[],1),
            new("party farewell",91,18011800,92,[71300083,11020600,71300078,71300090,71300091,71300081]),
            new("return",109,18010000,110,[71300069]),new("return repeat",111,18010100,112,[71300070,11026107])],
        _=>throw new ArgumentOutOfRangeException(nameof(id))};
    static string ExpectedHash(int id)=>id switch {6070=>"57acc94e02aa0af6b63730d2fd9faf1b470f12fc1aa54360c5d7aee6cdd3d683",6080=>"a48ba425eb27b21603bf5bf97c61430525e5e3bb1ffda84f6d7a49ecc6e963a2",6090=>"a6ae56ee0f4ea57de29678c05aa8786c5cad4c32366413809b9f0f2e9f1ccd52",6100=>"9ef0b5d06e1e04dc52e1f796e0d17472b5146ccb532f5865287312a42ef469a0",_=>throw new ArgumentOutOfRangeException(nameof(id))};
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Thorolund dialogue: "+why);}
    static int N(PARAM.Row row,string field)=>Convert.ToInt32(row[field].Value);
    static int Constant(byte[] b)=>b.Length==2&&b[0]<=127&&b[1]==0xa1?b[0]-64:b.Length==6&&b[0]==0x82&&b[5]==0xa1?BitConverter.ToInt32(b,1):throw new InvalidDataException("Oswald ESD constant changed");
    static string? Language(string name)=>name.ToLowerInvariant() switch {"engus" or "enggb"=>"ENGLISH","deude"=>"GERMAN","frafr"=>"FRENCH","itait"=>"ITALIAN","jpnjp" or "japanese"=>"JAPANESE","korkr"=>"KOREAN","polpl"=>"POLISH","rusru"=>"RUSSIAN","spaes"=>"SPANISH","spaar"=>"NSPANISH","porpt" or "porbr"=>"PORTUGUESE","zhocn"=>"SCHINESE","zhotw"=>"TCHINESE",_=>null};
    internal static Result Export(Func<string,byte[]?> source,Func<string,byte[]> native,Func<string,byte[]?> composed,
        string nativeRoot,string definitions,Dictionary<string,byte[]> files,int sourceEntity,string model,int talk,int firstRow,int firstVoice) {
        byte[] ReadSource(string path)=>source(path)??throw new FileNotFoundException("Thorolund source dialogue unavailable",path);
        byte[] Read(string path)=>files.TryGetValue(path,out var bytes)?bytes:composed(path)??native(path);
        const string sourceMap="m10_02_00_00",targetMap="m40_02_00_00";
        int sourceID=102000+sourceEntity/10;var selections=Select(sourceEntity);
        var file=BND3.Read(ReadSource("script/talk/"+sourceMap+".talkesdbnd.dcx")).Files.Single(f=>Leaf(f.Name)=="t"+sourceID+".esd");
        string hash=Convert.ToHexStringLower(SHA256.HashData(file.Bytes.Span));
        Need(hash==ExpectedHash(sourceEntity),"source graph changed");
        var sourceStates=ESD.Read(file.Bytes).StateGroups[1];
        foreach(var b in selections)Need(sourceStates[b.State].EntryCommands.Any(c=>c.CommandBank==1&&c.CommandID==1&&Constant(c.Arguments[0])==b.Row),"source speech changed");
        foreach(var b in selections.Where(b=>b.Completion>=0))foreach(int flag in b.Flags)
            Need(sourceStates[b.Completion].EntryCommands.Any(c=>c.CommandBank==1&&c.CommandID==11&&c.Arguments.Select(Constant).SequenceEqual(new[]{flag,1})),"source speech completion changed");
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
            for(int id=b.Row;id<b.Row+b.Limit;id++) {
                var original=sourceRows.Rows.SingleOrDefault(r=>r.ID==id);if(original==null)break;
                Need(N(original,"returnPos")==0&&N(original,"eventId")==-1&&N(original,"reactionId")==-1,"unported TalkParam continuation");
                int target=first+count,msg=N(original,"msgId");Need(english.TryGetValue(msg,out string? fallback)&&!string.IsNullOrWhiteSpace(fallback),"source subtitle missing");
                Need(!rows.Rows.Any(r=>r.ID==target)&&locales.All(l=>!l.Text.Entries.Any(e=>e.ID==target)),"owned speech reservation occupied");
                var row=new PARAM.Row(target,"Imported Thorolund: "+b.Name+" ["+id+"]",definition);foreach(var c in row.Cells)c.Value=-1;
                row["msgId"].Value=target;row["voiceId"].Value=-1;row["returnPos"].Value=0;rows.Rows.Add(row);voiceRows.Add((row,N(original,"voiceId")));
                foreach(var locale in locales){string? value=locale.Language==null?null:Text(locale.Language)?.GetValueOrDefault(msg);locale.Text.Entries.Add(new(locale.Text,target,string.IsNullOrWhiteSpace(value)?fallback:value));}count++;
            }
            Need(count>0&&(b.Limit<19?count==b.Limit:count<19),"unbounded/empty source sequence");speeches.Add(new(b.Name,b.State,b.Row,first,count,b.Flags));
        }
        var donor=ESD.Read(BND4.Read(native("script/talk/m24_00_00_00.talkesdbnd.dcx")).Files.Single(f=>Leaf(f.Name)=="t240000.esd").Bytes);
        string pathTalk="script/talk/"+targetMap+".talkesdbnd.dcx";var bankTalk=BND4.Read(Read(pathTalk));
        Need(!bankTalk.Files.Any(f=>Leaf(f.Name)=="t"+talk+".esd"),"owned talk actor occupied");
        var esd=Build(donor,sourceEntity,talk,speeches);var encoded=esd.Write();var check=ESD.Read(encoded);Need(check.LongFormat&&check.DarkSoulsCount==2&&check.StateGroups.Count==1,"native ESD readback");
        var carrier=bankTalk.Files[0];string prefix=carrier.Name[..(carrier.Name.LastIndexOfAny(['\\','/'])+1)];bankTalk.Files.Add(new(carrier.Flags,bankTalk.Files.Max(f=>f.ID)+1,prefix+"t"+talk+".esd",encoded));
        byte[]? Optional(string path)=>files.TryGetValue(path,out var bytes)?bytes:composed(path)??(File.Exists(Path.Combine(nativeRoot,path))?native(path):null);
        var voices=MapTalkVoices.Export([new(model,voiceRows.Select(r=>r.Voice).ToArray())],source,Optional,(path,bytes)=>files[path]=bytes,firstVoice:firstVoice);
        var ids=voices.Clips.ToDictionary(c=>c.SourceVoice,c=>c.NativeVoice);foreach(var v in voiceRows)v.Row["voiceId"].Value=ids[v.Voice];
        talkParam.Bytes=MapCharacters.WriteRows(rows);files["param/gameparam/gameparam.parambnd.dcx"]=game.Write(game.Compression);files[pathTalk]=bankTalk.Write(bankTalk.Compression);
        foreach(var locale in locales){locale.File.Bytes=locale.Text.Write();files[locale.Path]=locale.Binder.Write(locale.Binder.Compression);}
        return new(sourceEntity,talk,speeches,voices,locales.Count,hash);
    }
    static byte[] V(int n)=>n is >=-64 and <=63?[(byte)(n+64)]:[0x82,..BitConverter.GetBytes(n)];
    static byte[] E(byte[] b)=>[..b,0xa1];static byte[] F(int n,params byte[][] args)=>[..V(n),..args.SelectMany(a=>a),(byte)(0x84+args.Length),0xa6];
    static byte[] Eq(byte[] a,byte[] b)=>[..a,..b,0x95];static byte[] All(byte[] a,byte[] b)=>[..a,..b,0x98];static byte[] Any(byte[] a,byte[] b)=>[..a,..b,0x99];
    static ESD.CommandCall C(int n,params int[] args)=>new(1,n,args.Select(n=>E(V(n))).ToArray());
    internal static ESD Build(ESD template,int sourceEntity,int talk,IReadOnlyList<Speech> speeches) {
        var esd=new ESD(true,2){Name="t"+talk,Unk70=template.Unk70,Unk74=template.Unk74,Unk78=template.Unk78,Unk7C=template.Unk7C};var states=new Dictionary<long,ESD.State>();esd.StateGroups.Add(1,states);
        foreach(int i in new[]{0,1,2,3,4})states.Add(i,new());
        void Go(long a,long b,byte[] expr)=>states[a].Conditions.Add(new(b,E(expr)));
        byte[] Flag(int source,int value)=>Eq(F(15,V(MapNpcQuestState.Flag(source))),V(value));
        var abort=Any(Any(Eq(F(3),V(1)),Eq(F(19),V(1))),[..F(1),..V(5),0x94]);
        var targets=new Dictionary<string,int>();
        for(int index=0;index<speeches.Count;index++) {
            var speech=speeches[index];int start=100+index*2,done=start+1;targets.Add(speech.Name,start);states.Add(start,new());states.Add(done,new());
            states[start].EntryCommands.Add(C(1,speech.NativeRow,-1,-1));Go(start,3,abort);Go(start,done,Eq(F(57,V(0)),V(1)));
            foreach(int flag in speech.CompletionFlags)states[done].EntryCommands.Add(C(11,MapNpcQuestState.Flag(flag),1));Go(done,3,V(1));
        }
        Go(0,1,V(1));int present=sourceEntity switch{6070=>1171,6090=>1211,6100=>1221,_=>1202};
        byte[] available=Flag(present,1);if(sourceEntity==6080)available=Any(available,Flag(1193,1));Go(1,2,All(available,F(56,V(6000))));
        void Branch(string name,byte[] condition)=>Go(2,targets[name],condition);
        switch(sourceEntity) {
            case 6070:Branch("introduction",Flag(71020080,0));Branch("repeat",V(1));break;
            case 6090:
                Branch("farewell",All(Flag(11020600,1),Flag(71310024,1)));
                Branch("prayer",All(Flag(71310024,1),Flag(11026109,0)));
                Branch("mission",Flag(71310023,1));Branch("introduction",V(1));break;
            case 6100:
                Branch("farewell",Flag(11020600,1));Branch("repeat",All(Flag(71310032,1),Flag(11026110,0)));Branch("greeting",V(1));break;
            case 6080:
                Branch("introduction",Flag(71300065,0));Branch("warm greeting",All(Flag(1202,1),Flag(71300066,0)));
                Branch("party farewell",All(All(Flag(1202,1),Flag(1171,1)),Flag(71300083,0)));
                Branch("return",All(Flag(1193,1),Flag(71300069,0)));Branch("return repeat",Flag(1193,1));Branch("repeat",V(1));break;
            default:throw new InvalidDataException("Unowned Thorolund dialogue");
        }
        foreach(var c in new[]{C(71),C(8,0),C(9),C(36)})states[3].EntryCommands.Add(c);Go(3,4,V(1));Go(4,1,Eq(F(56,V(6000)),V(0)));return esd;
    }
}
