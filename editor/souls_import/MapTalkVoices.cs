// SPDX-License-Identifier: GPL-2.0-or-later
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;

// Convert only the selected Firelink speech samples. Retail native events,
// wavebanks and original game assets are inputs, never export destinations.
internal static class MapTalkVoices {
    internal sealed record Owner(string Model,IReadOnlyList<int> SourceVoices);
    internal sealed record Clip(int SourceVoice,int NativeVoice,int SampleIndex,int Frames,int Rate,int Channels,
        double DurationSeconds,string SourceSample,string PcmSha256);
    internal sealed record Project(string Model,string BankName,IReadOnlyList<Clip> Clips,string FevSha256,string FsbSha256);
    internal sealed record Result(string Codec,IReadOnlyList<Project> Projects,IReadOnlyList<Clip> Clips,IReadOnlyList<string> Limitations);
    internal sealed record Prepared(byte[] Bank,byte[] Hash,IReadOnlyList<Clip> Clips);
    sealed record SourceClip(string Name,int Offset,int Bytes,int Frames,int Rate,int Channels);
    internal const int FirstVoice=249970000;
    static readonly string[] BankSuffixes=["","_eng","_ded","_esa","_ese","_frf","_iti","_jaj","_ptb"];

    internal static Result Export(IReadOnlyList<Owner> owners,Func<string,byte[]?> readSource,
        Func<string,byte[]?> readNative,Action<string,byte[]> write) {
        byte[] Source(string p)=>readSource(p)??throw new FileNotFoundException("Source voice asset unavailable",p);
        byte[] Native(string p)=>readNative(p)??throw new FileNotFoundException("Native voice asset unavailable",p);
        if(owners.Count is <1 or >10||owners.Select(o=>o.Model).Distinct(StringComparer.Ordinal).Count()!=owners.Count)
            throw new InvalidDataException("Invalid imported voice project ownership");
        var allocated=new HashSet<int>();
        foreach(var owner in owners) {
            if(string.IsNullOrEmpty(owner.Model)||owner.Model.Length!=5||owner.Model[0]!='c'||!owner.Model.AsSpan(1).ToArray().All(char.IsAsciiDigit)||
                !int.TryParse(owner.Model.AsSpan(1),out int id)||id>8999)
                throw new InvalidDataException("Native character sound IDs must fit c0000..c8999: "+owner.Model);
            if(owner.SourceVoices.Count==0||owner.SourceVoices.Distinct().Any(v=>!allocated.Add(v)))
                throw new InvalidDataException("A source voice is empty or has multiple imported actor owners");
            string bank="sprj_"+owner.Model;
            foreach(string suffix in new[]{".fev",".itl"}.Concat(BankSuffixes.Select(s=>s+".fsb")))
                if(readNative("sound/"+bank+suffix)!=null)throw new InvalidDataException("Native character sound project is occupied: "+bank+suffix);
        }
        var original=Native("sound/sprj_sm24.fev");
        var defaultProject=Native("sound/sprj_c7110.fev");
        var source=Source("sound/frpg_sm10.fsb");var header=Native("sound/sprj_sm24_eng.fsb");
        var pending=new Dictionary<string,byte[]>(StringComparer.Ordinal);var projects=new List<Project>();var clips=new List<Clip>();
        int nextVoice=FirstVoice;
        foreach(var owner in owners) {
            var selected=owner.SourceVoices.Distinct().ToArray();
            var prepared=Prepare(source,header,selected,nextVoice);nextVoice=checked(nextVoice+prepared.Clips.Count);
            var samples=prepared.Clips.Select(c=>new NativeTalkBank.Sample(c.NativeVoice,$"v{c.NativeVoice}",c.Frames,c.Rate,c.SampleIndex)).ToArray();
            string bank="sprj_"+owner.Model;var fev=NativeTalkBank.BuildOwned(original,defaultProject,owner.Model,prepared.Hash,samples);
            // The English default bank pairs with NpcParam.isMultilingual=false,
            // so native character preload registers this unsuffixed bank name.
            pending.Add("sound/"+bank+".fev",fev);pending.Add("sound/"+bank+".fsb",prepared.Bank);
            projects.Add(new(owner.Model,bank,prepared.Clips,Convert.ToHexString(SHA256.HashData(fev)),Convert.ToHexString(SHA256.HashData(prepared.Bank))));
            clips.AddRange(prepared.Clips);
        }
        // Complete the whole owned-project batch before any mod-only write.
        foreach(var (path,bytes) in pending)write(path,bytes);
        return new("FSB5 PCM16",projects,clips,[
            "Original English source speech is used for every native audio language; subtitles retain their source localizations.",
            "Each imported actor owns one English default-language native character project paired with NpcParam.isMultilingual=false. Retail sound projects and wavebanks are not exported.",
            "Only selected source speech is converted. Source talk animations, complete NPC quests and other sound banks are not translated."
        ]);
    }

    internal static Prepared Prepare(byte[] source,byte[] native,IReadOnlyList<int> sourceVoices,int firstVoice=FirstVoice) {
        if(source.Length<48||Encoding.ASCII.GetString(source,0,4)!="FSB4"||native.Length<60||Encoding.ASCII.GetString(native,0,4)!="FSB5")
            throw new InvalidDataException("Expected source FSB4 and native FSB5 voice banks");
        if(BitConverter.ToInt32(native,4)!=1)throw new InvalidDataException("Unexpected native FSB5 version");
        var requested=sourceVoices.Distinct().ToArray();
        if(requested.Length is <1 or >100||requested.Any(v=>v<=0)||firstVoice<=0||firstVoice>999999999-requested.Length+1)throw new InvalidDataException("Invalid bounded source voice selection");
        int count=BitConverter.ToInt32(source,4),headers=BitConverter.ToInt32(source,8);
        if(count is <1 or >100000||headers<0||48L+headers>source.Length)throw new InvalidDataException("Invalid source voice header table");
        int at=48,data=48+headers;var sourceClips=new Dictionary<string,SourceClip>(StringComparer.Ordinal);
        for(int i=0;i<count;i++) {
            if(at+80>48+headers)throw new InvalidDataException("Truncated source voice header");
            int size=BitConverter.ToUInt16(source,at);if(size<80||at+size>48+headers)throw new InvalidDataException("Unsupported source voice header size");
            int end=Array.IndexOf(source,(byte)0,at+2,30);if(end<0)throw new InvalidDataException("Unterminated source voice name");
            string name=Encoding.ASCII.GetString(source,at+2,end-at-2);
            int frames=BitConverter.ToInt32(source,at+32),bytes=BitConverter.ToInt32(source,at+36),rate=BitConverter.ToInt32(source,at+52),channels=BitConverter.ToUInt16(source,at+62);
            if(bytes<0||data+(long)bytes>source.Length)throw new InvalidDataException("Source voice sample exceeds its bank");
            if(!sourceClips.TryAdd(name,new(name,data,bytes,frames,rate,channels)))throw new InvalidDataException("Repeated source voice sample");
            data=checked(data+bytes);at+=size;
        }
        if(data!=source.Length)throw new InvalidDataException("Source voice data size differs from its headers");
        using var sampleHeaders=new MemoryStream();using var names=new MemoryStream();using var audio=new MemoryStream();
        using var headerWriter=new BinaryWriter(sampleHeaders);using var nameWriter=new BinaryWriter(names);
        var clips=new List<Clip>();var offsets=new List<int>();
        for(int i=0;i<requested.Length;i++) {
            int sourceVoice=requested[i],nativeVoice=checked(firstVoice+i);string sample=$"v{sourceVoice:D9}.wav";
            if(!sourceClips.TryGetValue(sample,out var clip))throw new InvalidDataException("Source speech sample unavailable: "+sample);
            if(clip.Frames is <1152 or >44100*120||clip.Frames%1152!=0||clip.Rate!=44100||clip.Channels!=1)
                throw new InvalidDataException("Unsupported source speech sample format: "+sample);
            var mp3=UnpadMp3(source.AsSpan(clip.Offset,clip.Bytes),clip.Frames,clip.Rate);
            var pcm=Decode(mp3);if(pcm.Length!=clip.Frames*2)throw new InvalidDataException("Decoded source voice frame count changed: "+sample);
            if(audio.Position%32!=0)throw new InvalidDataException("Native PCM voice offset is not aligned");
            ulong mode=((ulong)clip.Frames<<34)|((ulong)(audio.Position/32)<<7)|(8ul<<1); // mono, 44.1kHz, no loop
            headerWriter.Write(mode);offsets.Add(checked(requested.Length*4+(int)names.Position));nameWriter.Write(Encoding.ASCII.GetBytes($"v{nativeVoice}\0"));
            audio.Write(pcm);while(audio.Position%32!=0)audio.WriteByte(0);
            clips.Add(new(sourceVoice,nativeVoice,i,clip.Frames,clip.Rate,clip.Channels,clip.Frames/(double)clip.Rate,sample,Convert.ToHexString(SHA256.HashData(pcm))));
        }
        using var nameTable=new MemoryStream();using(var writer=new BinaryWriter(nameTable,Encoding.UTF8,true)){foreach(int offset in offsets)writer.Write(offset);writer.Write(names.ToArray());}
        byte[] header=native[..60];byte[] identity=MD5.HashData(Encoding.UTF8.GetBytes("Imported Firelink speech PCM16: "+string.Join(',',clips.Select(c=>c.SourceVoice+":"+c.PcmSha256))));
        BitConverter.GetBytes(clips.Count).CopyTo(header,8);BitConverter.GetBytes(checked((int)sampleHeaders.Length)).CopyTo(header,12);
        BitConverter.GetBytes(checked((int)nameTable.Length)).CopyTo(header,16);BitConverter.GetBytes(checked((int)audio.Length)).CopyTo(header,20);
        BitConverter.GetBytes(2).CopyTo(header,24);identity.CopyTo(header,36);
        using var bank=new MemoryStream();bank.Write(header);sampleHeaders.Position=0;sampleHeaders.CopyTo(bank);nameTable.Position=0;nameTable.CopyTo(bank);audio.Position=0;audio.CopyTo(bank);
        return new(bank.ToArray(),identity,clips);
    }

    internal static byte[] UnpadMp3(ReadOnlySpan<byte> data,int frames,int rate) {
        int[] rates=[44100,48000,32000],bitrates=[0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0];
        using var stream=new MemoryStream();int at=0;
        for(int i=0;i<frames/1152;i++) {
            if(at+4>data.Length)throw new InvalidDataException("Truncated MPEG voice frame");
            uint h=System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(data[at..]);int version=(int)(h>>19)&3,layer=(int)(h>>17)&3,bitrate=(int)(h>>12)&15,rateIndex=(int)(h>>10)&3;
            if(h>>21!=0x7ff||version!=3||layer!=1||bitrate is 0 or 15||rateIndex==3||rates[rateIndex]!=rate)
                throw new InvalidDataException("Unsupported MPEG voice frame");
            int size=144000*bitrates[bitrate]/rate+(int)((h>>9)&1);
            if(at+(long)size>data.Length)throw new InvalidDataException("MPEG voice frame exceeds sample");
            stream.Write(data.Slice(at,size));at=checked(at+((size+3)&~3));
        }
        if(at>data.Length||data[at..].IndexOfAnyExcept((byte)0)>=0)throw new InvalidDataException("Unexpected MPEG voice sample tail");
        return stream.ToArray();
    }

    static byte[] Decode(byte[] mp3) {
        string decoder=Environment.GetEnvironmentVariable("BBMAP_FFMPEG")??
            new[]{"/opt/homebrew/bin/ffmpeg","/usr/local/bin/ffmpeg"}.FirstOrDefault(File.Exists)??"ffmpeg";
        var start=new ProcessStartInfo(decoder){UseShellExecute=false,RedirectStandardInput=true,RedirectStandardOutput=true,RedirectStandardError=true};
        foreach(string arg in new[]{"-v","error","-f","mp3","-i","pipe:0","-f","s16le","-acodec","pcm_s16le","-ar","44100","-ac","1","pipe:1"})start.ArgumentList.Add(arg);
        using var process=Process.Start(start)??throw new IOException("Could not start ffmpeg for source voice conversion");
        using var output=new MemoryStream();var read=process.StandardOutput.BaseStream.CopyToAsync(output);var errors=process.StandardError.ReadToEndAsync();
        process.StandardInput.BaseStream.Write(mp3);process.StandardInput.Close();process.WaitForExit();read.GetAwaiter().GetResult();string message=errors.GetAwaiter().GetResult();
        if(process.ExitCode!=0)throw new InvalidDataException("Source voice decoder failed: "+message.Trim());
        return output.ToArray();
    }
}
