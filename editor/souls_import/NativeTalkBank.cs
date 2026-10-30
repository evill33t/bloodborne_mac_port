// SPDX-License-Identifier: GPL-2.0-or-later
using System.Security.Cryptography;
using System.Text;

// Build an owned character sound project without editing retail projects.
// Reuse the reviewed Bloodborne sm24/v241900000 voice event and c7110
// default-language memory-bank schema; retain their provenance and native data.
internal static class NativeTalkBank {
    internal sealed record Sample(int VoiceID,string WaveName,int Frames,int Rate,int SampleIndex);
    sealed record Chunk(string Tag,int At,int Size) {internal int End=>checked(At+8+Size+(Size&1));}
    sealed record Bank(int At,int End,int NameAt,string Name);
    sealed record Sound(int At,int Definition);
    sealed record Envelope(string Name,int Points);
    sealed record Event(int At,int End,int Kind,int NameIndex,string Name,IReadOnlyList<Sound> Sounds,
        int Layers,IReadOnlyList<Envelope> Envelopes,IReadOnlyList<string> Parameters,IReadOnlyList<string> Properties,int CategoryChars,string Group);
    sealed record Group(string Name,int End,int CountAt,int Count);
    sealed record Wave(string Path,int Bank,int Sample,int Duration);
    sealed record Definition(int At,int End,int NameIndex,int Setting,IReadOnlyList<Wave> Waves);
    static readonly UTF8Encoding Utf8=new(false,true);
    static void Require(bool condition,string message){if(!condition)throw new InvalidDataException("Native voice bank: "+message);}
    static int Int(byte[] b,int at){Require(at>=0&&at<=b.Length-4,"truncated integer");return System.Buffers.Binary.BinaryPrimitives.ReadInt32LittleEndian(b.AsSpan(at,4));}
    static void Put(byte[] b,int at,int value){Require(at>=0&&at<=b.Length-4,"truncated integer patch");System.Buffers.Binary.BinaryPrimitives.WriteInt32LittleEndian(b.AsSpan(at,4),value);}
    static byte[] Slice(byte[] b,int at,int count){Require(at>=0&&count>=0&&at<=b.Length-count,"truncated data");return b.AsSpan(at,count).ToArray();}
    static bool Same(byte[] a,int at,byte[] b,int bt,int count)=>at>=0&&bt>=0&&count>=0&&at<=a.Length-count&&bt<=b.Length-count&&a.AsSpan(at,count).SequenceEqual(b.AsSpan(bt,count));
    static List<Chunk> Chunks(byte[] b,int at,int end) {
        var result=new List<Chunk>();Require(at>=0&&end>=at&&end<=b.Length,"invalid chunk bounds");
        while(at<end){Require(at<=end-8,"truncated chunk header");int size=Int(b,at+4);Require(size>=0&&size<=end-at-8,"invalid chunk size");var c=new Chunk(Encoding.ASCII.GetString(b,at,4),at,size);Require(c.End<=end,"invalid chunk padding");result.Add(c);at=c.End;}
        return result;
    }
    sealed class Reader {
        readonly byte[] bytes;readonly int limit;internal int At;
        internal Reader(byte[] b,int at,int end){bytes=b;At=at;limit=end;}
        internal byte[] Take(int count){Require(count>=0&&At<=limit-count,"truncated project record");var value=Slice(bytes,At,count);At+=count;return value;}
        internal void Skip(int count){Require(count>=0&&At<=limit-count,"truncated project record");At+=count;}
        internal int Number()=>System.Buffers.Binary.BinaryPrimitives.ReadInt32LittleEndian(Take(4));
        internal int Count(int maximum=100000){int n=Number();Require(n>=0&&n<=maximum,"record count exceeds budget");return n;}
        internal int Short()=>System.Buffers.Binary.BinaryPrimitives.ReadUInt16LittleEndian(Take(2));
        internal string Text(){int n=Count(65536);var text=Take(n);if(n==0)return "";Require(text[^1]==0&&text.AsSpan(0,n-1).IndexOf((byte)0)<0,"invalid project string");return Utf8.GetString(text,0,n-1);}
    }
    sealed class Project {
        internal readonly byte[] Bytes;
        internal readonly Chunk List,Legacy,StringsChunk,ObjectChunk;
        internal readonly List<Chunk> Children;
        internal readonly List<Bank> Banks=[];
        internal readonly List<Event> Events=[];
        internal readonly List<Group> Groups=[];
        internal readonly List<Definition> Definitions=[];
        internal readonly Dictionary<int,(int At,int Value)> Objects=[];
        internal readonly List<string> Strings=[];
        internal readonly List<int> StringOffsets=[];
        internal readonly byte[] StringBlob;
        internal readonly string Name;
        internal readonly int Languages,BankCountAt,PoolAt,PoolCount,DefinitionCountAt,ParsedEnd,WaveCount;
        internal Project(byte[] bytes) {
            Bytes=bytes;Require(bytes.Length is >=512 and <=32*1024*1024&&Encoding.ASCII.GetString(bytes,0,4)=="RIFF"&&Int(bytes,4)==bytes.Length-8&&Encoding.ASCII.GetString(bytes,8,4)=="FEV ","unsupported RIFF project");
            List=Chunks(bytes,12,bytes.Length).Single(c=>c.Tag=="LIST");Require(Encoding.ASCII.GetString(bytes,List.At+8,4)=="PROJ","missing project list");
            Children=Chunks(bytes,List.At+12,List.At+8+List.Size);Require(Children.Select(c=>c.Tag).Distinct().Count()==Children.Count,"duplicate project chunks");
            Legacy=Children.Single(c=>c.Tag=="LGCY");StringsChunk=Children.Single(c=>c.Tag=="STRR");ObjectChunk=Children.Single(c=>c.Tag=="OBCT");
            int str=StringsChunk.At+8;int count=Int(bytes,str);Require(count is >=1 and <=100000&&4L+count*4L<=StringsChunk.Size,"invalid string table");int blob=checked(str+4+count*4);StringBlob=Slice(bytes,blob,str+StringsChunk.Size-blob);
            for(int i=0;i<count;i++){int offset=Int(bytes,str+4+i*4);Require(offset>=0&&offset<StringBlob.Length,"invalid string offset");int end=Array.IndexOf(StringBlob,(byte)0,offset);Require(end>=offset&&end-offset<=65536,"unterminated string");Strings.Add(Utf8.GetString(StringBlob,offset,end-offset));StringOffsets.Add(offset);}
            var objects=new Reader(bytes,ObjectChunk.At+8,ObjectChunk.At+8+ObjectChunk.Size);int objectCount=objects.Count(128);for(int i=0;i<objectCount;i++){int kind=objects.Number(),at=objects.At,value=objects.Number();Require(value>=0&&Objects.TryAdd(kind,(at,value)),"invalid object allocation table");}Require(objects.At==ObjectChunk.At+8+ObjectChunk.Size,"object table trailing data");
            var r=new Reader(bytes,Legacy.At+16,Legacy.At+8+Legacy.Size);Name=r.Text();BankCountAt=r.At;int banks=r.Count(32);Languages=r.Count(32);
            for(int i=0;i<banks;i++){int at=r.At;r.Skip(8+Languages*12);int length=r.Count(256),nameAt=r.At;var raw=r.Take(length);Require(length>0&&raw[^1]==0,"invalid wavebank name");Banks.Add(new(at,r.At,nameAt,Utf8.GetString(raw,0,length-1)));}
            void Category(int depth){Require(depth<=16,"category nesting exceeds budget");r.Text();r.Skip(16);int children=r.Count(256);for(int i=0;i<children;i++)Category(depth+1);}
            List<string> Properties(){var result=new List<string>();int n=r.Count(1024);for(int i=0;i<n;i++){result.Add(r.Text());int kind=r.Number();if(kind is 0 or 1)r.Skip(4);else if(kind==2)r.Text();else throw new InvalidDataException("Native voice bank: unsupported property");}return result;}
            string String(int index){Require(index>=0&&index<Strings.Count,"invalid string identity");return Strings[index];}
            Sound ReadSound(){int at=r.At,definition=r.Short();r.Skip(56);return new(at,definition);}
            Envelope ReadEnvelope(){r.Skip(4);string name=r.Text();r.Skip(12);int points=r.Count(65536);r.Skip(checked(points*4+8));return new(name,points);}
            void ReadEvent(string group){
                int at=r.At,kind=r.Number(),nameIndex=r.Number();string name=String(nameIndex);r.Skip(16+144);var sounds=new List<Sound>();var envelopes=new List<Envelope>();var parameters=new List<string>();var properties=new List<string>();int layers=0;
                if((kind&0x18)==8){layers=r.Count(1024);for(int i=0;i<layers;i++){r.Skip(6);int sn=r.Short(),en=r.Short();Require(sn<=1024&&en<=1024,"layer exceeds budget");for(int j=0;j<sn;j++)sounds.Add(ReadSound());for(int j=0;j<en;j++)envelopes.Add(ReadEnvelope());}int pn=r.Count(1024);for(int i=0;i<pn;i++){parameters.Add(String(r.Number()));r.Skip(24);int n=r.Count(65536);r.Skip(checked(n*4));}properties=Properties();}
                else if((kind&0x18)==16){r.Skip(4);sounds.Add(ReadSound());}
                else throw new InvalidDataException("Native voice bank: unsupported event kind");
                int refs=r.Count(256),chars=0;for(int i=0;i<refs;i++)chars=checked(chars+Utf8.GetByteCount(r.Text())+1);
                Events.Add(new(at,r.At,kind,nameIndex,name,sounds,layers,envelopes,parameters,properties,chars,group));
            }
            void ReadGroup(int depth){Require(depth<=16,"event group nesting exceeds budget");string name=String(r.Number());Properties();int groups=r.Count(1024),countAt=r.At,n=r.Count();for(int i=0;i<groups;i++)ReadGroup(depth+1);for(int i=0;i<n;i++)ReadEvent(name);Groups.Add(new(name,r.At,countAt,n));}
            Category(0);int roots=r.Count(1024);for(int i=0;i<roots;i++)ReadGroup(0);
            PoolCount=r.Count(4096);PoolAt=r.At;r.Skip(checked(PoolCount*74));DefinitionCountAt=r.At;int defs=r.Count(65535);int waveCount=0;
            for(int i=0;i<defs;i++){int at=r.At,nameIndex=r.Number();String(nameIndex);int setting=r.Number();Require(setting>=0&&setting<PoolCount,"invalid playback setting identity");int wn=r.Count(65535);var waves=new List<Wave>();for(int j=0;j<wn;j++){int kind=r.Number();r.Skip(4);if(kind==0){string path=r.Text();waves.Add(new(path,r.Number(),r.Number(),r.Number()));waveCount++;}else if(kind==1)r.Skip(8);else Require(kind is 2 or 3,"unsupported wave record");}Definitions.Add(new(at,r.At,nameIndex,setting,waves));}
            ParsedEnd=r.At;WaveCount=waveCount;
        }
    }
    static byte[] Text(string value){var b=Utf8.GetBytes(value);using var s=new MemoryStream();using var w=new BinaryWriter(s);w.Write(checked(b.Length+1));w.Write(b);w.Write((byte)0);return s.ToArray();}
    static byte[] ChunkBytes(string tag,byte[] payload){using var s=new MemoryStream();using var w=new BinaryWriter(s);w.Write(Encoding.ASCII.GetBytes(tag));w.Write(payload.Length);w.Write(payload);if((payload.Length&1)!=0)w.Write((byte)0);return s.ToArray();}
    static byte[] Join(IEnumerable<byte[]> records){using var s=new MemoryStream();foreach(var b in records)s.Write(b);return s.ToArray();}

    // A character resource loads and registers its own basename FSB. Native
    // character bank IDs must be below9000. The PCM control used c7110's
    // memory-bank mode and single default language; additional streamed banks
    // in sm24 did not establish working source speech.
    internal static byte[] BuildOwned(byte[] nativeFEV,byte[] nativeDefaultProject,string model,byte[] bankHash,IReadOnlyList<Sample> samples) {
        Require(model.Length==5&&model[0]=='c'&&model.AsSpan(1).IndexOfAnyExceptInRange('0','9')<0&&int.Parse(model[1..])<9000,"invalid native character sound owner");
        Require(bankHash.Length==16&&samples.Count is >=1 and <=512&&samples.Select(s=>s.VoiceID).Distinct().Count()==samples.Count&&samples.Select(s=>s.SampleIndex).Distinct().Count()==samples.Count,"invalid owned voice allocation");
        foreach(var s in samples)Require(s.VoiceID is >=240000000 and <=249999999&&s.WaveName=="v"+s.VoiceID&&s.Frames>0&&s.Rate is >=8000 and <=192000&&s.SampleIndex is >=0 and <=65535&&(double)s.Frames/s.Rate<=1800,"invalid owned source voice sample");
        var project=new Project(nativeFEV);var defaults=new Project(nativeDefaultProject);
        Require(project.Name=="sprj_sm24"&&project.Groups.Count==1&&project.Objects.Count==33&&Enumerable.Range(0,33).All(project.Objects.ContainsKey),"unreviewed owned voice scaffold");
        Require(samples.All(s=>!project.Strings.Contains("v"+s.VoiceID)),"owned voice identity collides with a retail event");
        Require(defaults.Name=="sprj_c7110"&&defaults.Banks.Count==1&&defaults.Languages==1,"unreviewed default-language project");
        var language=defaults.Children.Single(c=>c.Tag=="LANG");var lr=new Reader(nativeDefaultProject,language.At+8,language.At+8+language.Size);
        Require(lr.Number()==1&&lr.Text()=="default"&&lr.Number()==0&&lr.At==language.At+8+language.Size,"unreviewed native default language");
        var defaultBank=defaults.Banks[0];Require(Int(nativeDefaultProject,defaultBank.At)==256&&defaultBank.NameAt-defaultBank.At-4==20,"unreviewed native memory wavebank");
        int defaultSuffix=Int(nativeDefaultProject,defaultBank.At+16);Require(defaultSuffix>=0&&defaultSuffix<defaults.Strings.Count&&defaults.Strings[defaultSuffix]=="","native memory bank has a language suffix");
        var donor=project.Events.Single(e=>e.Name=="v241900000");
        Require(donor.Kind==8&&donor.End-donor.At==541&&donor.Sounds.Count==1&&donor.Layers==1&&donor.Envelopes.SequenceEqual(new[]{new Envelope("",3),new Envelope("FMOD Lowpass Simple",2)})&&donor.Parameters.SequenceEqual(new[]{"(distance)"})&&donor.Properties.SequenceEqual(new[]{"playDistance","stopDistance","returnType","mixerNum","mixerID_0","UpdateDefilade"})&&donor.CategoryChars==6,"unreviewed owned voice event schema");
        var definition=project.Definitions[donor.Sounds[0].Definition];Require(definition.Setting==1&&definition.Waves.Count==1&&project.PoolCount>1,"unreviewed owned voice playback setting");
        var group=project.Groups.Single();int groupAt=group.CountAt-12,firstEvent=project.Events.Min(e=>e.At);
        Require(firstEvent==group.CountAt+4&&Int(nativeFEV,groupAt+4)==0&&Int(nativeFEV,groupAt+8)==0&&project.Strings[Int(nativeFEV,groupAt)]==group.Name,"owned voice group has additional scaffolding");
        string name="sprj_"+model;var strings=new List<string>(project.Strings);int groupIndex=strings.Count;strings.Add(model);
        var events=new List<byte[]>();var definitions=new List<byte[]>();var paths=new List<string>();
        foreach(var s in samples) {
            string eventName="v"+s.VoiceID,path="bank/"+name+"/"+s.WaveName+".wav";int nameIndex=strings.Count;strings.Add(eventName);int defIndex=strings.Count;strings.Add("/"+name+"/"+eventName);
            var e=Slice(nativeFEV,donor.At,donor.End-donor.At);Put(e,4,nameIndex);MD5.HashData(Encoding.ASCII.GetBytes("BBImport source voice "+name+"/"+eventName)).CopyTo(e,8);
            System.Buffers.Binary.BinaryPrimitives.WriteUInt16LittleEndian(e.AsSpan(donor.Sounds[0].At-donor.At,2),checked((ushort)events.Count));events.Add(e);
            int duration=checked((int)Math.Round((double)s.Frames*1000/s.Rate,MidpointRounding.ToEven));using var stream=new MemoryStream();using var w=new BinaryWriter(stream);
            w.Write(defIndex);w.Write(0);w.Write(1);w.Write(0);w.Write(100);w.Write(Text(path));w.Write(0);w.Write(s.SampleIndex);w.Write(duration);definitions.Add(stream.ToArray());paths.Add(path);
        }
        int empty=strings.IndexOf("");if(empty<0){empty=strings.Count;strings.Add("");}
        var descriptor=Join([Slice(nativeDefaultProject,defaultBank.At,20),Text(name)]);bankHash.AsSpan(0,8).CopyTo(descriptor.AsSpan(8,8));Put(descriptor,16,empty);
        int scaffoldAt=project.Banks[^1].End;var scaffold=Slice(nativeFEV,scaffoldAt,firstEvent-scaffoldAt);Put(scaffold,groupAt-scaffoldAt,groupIndex);Put(scaffold,group.CountAt-scaffoldAt,samples.Count);
        var playback=Slice(nativeFEV,project.PoolAt+74,74);int legacyEnd=project.Legacy.At+8+project.Legacy.Size;
        Require(legacyEnd-project.ParsedEnd==12&&Int(nativeFEV,project.ParsedEnd)==0&&Int(nativeFEV,project.ParsedEnd+4)==8&&Encoding.ASCII.GetString(nativeFEV,project.ParsedEnd+8,4)=="comp","unreviewed native project tail");
        byte[] legacy;using(var stream=new MemoryStream()){using var w=new BinaryWriter(stream);w.Write(checked(samples.Count*24));w.Write(checked(samples.Count*32));w.Write(Text(name));w.Write(1);w.Write(1);w.Write(descriptor);w.Write(scaffold);foreach(var e in events)w.Write(e);w.Write(1);w.Write(playback);w.Write(samples.Count);foreach(var d in definitions)w.Write(d);w.Write(Slice(nativeFEV,project.ParsedEnd,12));legacy=stream.ToArray();}
        var offsets=new List<int>(project.StringOffsets);using var blob=new MemoryStream();blob.Write(project.StringBlob);foreach(var s in strings.Skip(project.Strings.Count)){offsets.Add(checked((int)blob.Position));blob.Write(Utf8.GetBytes(s));blob.WriteByte(0);}
        byte[] strr;using(var stream=new MemoryStream()){using var w=new BinaryWriter(stream);w.Write(strings.Count);foreach(int offset in offsets)w.Write(offset);w.Write(blob.ToArray());strr=stream.ToArray();}
        // Exact allocation graph of the retained category scaffold plus cloned
        // voices. OBCT19 is the project name and20 the wavebank name. The empty
        // native Volume envelope name occupies7bytes; Lowpass occupies20.
        int n=samples.Count;var counts=Enumerable.Range(0,33).ToDictionary(i=>i,_=>0);
        foreach(var (kind,value) in new[]{(0,1),(1,1),(2,project.Objects[2].Value),(3,1),(4,6*n),(5,n),(6,2*n),(7,5*n),(8,n),(9,n),(11,n),(13,n),(17,n),
            (19,Utf8.GetByteCount(name)+1),(20,Utf8.GetByteCount(name)+1),(21,project.Objects[21].Value),(22,Utf8.GetByteCount(model)+1),(23,71*n),(25,11*n),(26,27*n),(27,11*n),(28,6*n),
            (30,paths.Sum(p=>Utf8.GetByteCount(p)+1)),(32,samples.Sum(s=>Utf8.GetByteCount("/"+name+"/v"+s.VoiceID)+1))})counts[kind]=value;
        var obct=Slice(nativeFEV,project.ObjectChunk.At+8,project.ObjectChunk.Size);foreach(var (kind,value) in counts)Put(obct,project.Objects[kind].At-project.ObjectChunk.At-8,value);
        var replacements=new Dictionary<string,byte[]>{{"OBCT",obct},{"PROP",Text(name)},{"LGCY",legacy},{"STRR",strr},{"LANG",Slice(nativeDefaultProject,language.At+8,language.Size)}};
        var list=Join(new[]{Encoding.ASCII.GetBytes("PROJ")}.Concat(project.Children.Select(c=>replacements.TryGetValue(c.Tag,out var payload)?ChunkBytes(c.Tag,payload):Slice(nativeFEV,c.At,c.End-c.At))));
        var output=Join([Slice(nativeFEV,0,project.List.At),ChunkBytes("LIST",list),Slice(nativeFEV,project.List.End,nativeFEV.Length-project.List.End)]);Put(output,4,output.Length-8);
        var result=new Project(output);Require(result.Name==name&&result.Groups.Count==1&&result.Groups[0].Name==model&&result.Events.Count==n&&result.Definitions.Count==n&&result.Banks.Count==1&&result.Banks[0].Name==name&&result.Languages==1&&result.PoolCount==1,"owned voice graph readback mismatch");
        Require(result.Strings.Take(project.Strings.Count).SequenceEqual(project.Strings)&&Same(output,result.PoolAt,playback,0,74)&&counts.All(p=>result.Objects[p.Key].Value==p.Value),"owned voice allocation/playback readback mismatch");
        for(int i=0;i<n;i++){var e=result.Events[i];var d=result.Definitions[i];var s=samples[i];int duration=checked((int)Math.Round((double)s.Frames*1000/s.Rate,MidpointRounding.ToEven));Require(e.Name=="v"+s.VoiceID&&e.Sounds.Single().Definition==i&&d.Setting==0&&d.Waves.Single()==new Wave(paths[i],0,s.SampleIndex,duration),"owned source voice link differs after serialization");}
        return output;
    }
}
