// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Text.RegularExpressions;

// Source poses for the three initial Firelink NPC appearances. These private
// idle slots retain native playback/physics; they do not import source NPC AI.
internal static class MapCharacterAnimations {
    internal sealed record Pose(string SourceModel,int SourceClipID,string SourceArchive,float Duration,int FrameCount,
        IReadOnlyList<int> NativeIdleSlots,bool Approximation,IReadOnlyList<string> Limitations);
    internal sealed record SourceClip(SourceAnimation Animation,string Archive);
    static readonly int[] IdleSlots=[0,1000000,2000000];
    static string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();

    internal static SourceClip Load(Func<string,byte[]?> read,string model,int id) {
        if(!Regex.IsMatch(model,@"\Ac\d{4}\z")||id is <0 or >9999)throw new ArgumentException("Invalid source NPC animation identifier");
        long total=0;
        (BND3 Bank,string Path)? Bank(string stem) {
            string path=$"chr/{stem}.anibnd.dcx";var bytes=read(path);
            if(bytes==null){path=path[..^4];bytes=read(path);}
            if(bytes==null)return null;
            if(bytes.Length is <1 or >128*1024*1024||(total+=bytes.Length)>256*1024*1024)
                throw new InvalidDataException("Source NPC animation archives exceed the read budget");
            return(BND3.Read(bytes),path);
        }
        var main=Bank(model)??throw new FileNotFoundException("Source NPC animation archive unavailable: "+model);
        var skeleton=main.Bank.Files.Where(f=>Leaf(f.Name).Equals("Skeleton.hkx",StringComparison.OrdinalIgnoreCase)).ToArray();
        if(skeleton.Length!=1)throw new InvalidDataException("Missing or ambiguous source NPC animation skeleton");
        // Main c0000 contains the skeleton/TAEs and placeholder TXT filenames
        // declaring split archives. Their text is an authoring instruction, not
        // a clip manifest. Search exact HKX names instead of guessing ID ranges.
        var stems=main.Bank.Files.Where(f=>f.ID is >=6000000 and <7000000).Select(f=>Leaf(f.Name))
            .Where(n=>n.StartsWith(model+"_",StringComparison.OrdinalIgnoreCase)&&Regex.IsMatch(n,@"\Ac\d{4}_[A-Za-z0-9_]+\.txt\z",RegexOptions.IgnoreCase))
            .Select(n=>n[..^4]).Distinct(StringComparer.OrdinalIgnoreCase).Order(StringComparer.Ordinal).ToArray();
        if(stems.Length>64)throw new InvalidDataException("Too many source NPC split animation archives");
        string clipName=$"a00_{id:D4}.hkx";var matches=new List<(byte[] Bytes,string Path)>();
        void Find((BND3 Bank,string Path) bank) {
            foreach(var f in bank.Bank.Files.Where(f=>Leaf(f.Name).Equals(clipName,StringComparison.OrdinalIgnoreCase)))
                matches.Add((f.Bytes.ToArray(),bank.Path));
        }
        Find(main);
        foreach(string stem in stems)if(Bank(stem) is {} bank)Find(bank);
        if(matches.Count!=1)throw new InvalidDataException("Missing or ambiguous source NPC animation: "+model+"/"+clipName);
        return new(SourceAnimation.Decode(skeleton[0].Bytes.ToArray(),matches[0].Bytes),matches[0].Path);
    }

    static bool DollIdleEvent(TAE.Event e) {
        var b=e.GetParameterBytes(false);
        int I(int at)=>BitConverter.ToInt32(b,at);
        if(e.Type==66&&b.Length==16&&I(0)==160&&b.AsSpan(4).IndexOfAnyExcept((byte)0)<0)return true;
        if(e.Type==67&&b.Length==16&&I(0)==153&&b.AsSpan(4).IndexOfAnyExcept((byte)0)<0)return true;
        if(e.Type==128&&b.Length==16&&I(0)==1&&I(4)==803006500&&b.AsSpan(8).IndexOfAnyExcept((byte)0)<0)return true;
        if(e.Type==313&&b.Length==16&&I(0)==0&&I(4)==0x3f800000&&I(8)==0x3f800000&&I(12)==0)return true;
        if(e.Type!=315||b.Length!=48||I(16) is <0 or >5||I(20)!=0||I(40)!=0||I(44)!=0)return false;
        return new[]{0,4,8,12,24,28,32,36}.All(at=>I(at)==unchecked((int)0xbf800000));
    }

    internal static Pose Apply(Func<string,byte[]?> readSource,MSB1.Part.Enemy source,string target,SkinBake skin,Dictionary<string,byte[]> files) {
        if(!Regex.IsMatch(target,@"\Ac\d{4}\z"))throw new ArgumentException("Invalid target NPC animation identifier");
        bool approximate=false;
        int clipID=(source.ModelName,source.CharaInitID,source.NPCParamID,source.InitAnimID) switch {
            ("c0000",6270,6270,7810)=>7810,
            ("c0000",6080,6080,-1)=>0,
            ("c2750",-1,275000,-1)=>0,
            _=>throw new NotSupportedException("Source NPC initial pose is not supported: "+source.Name)
        };
        var limitations=new List<string>();
        if(source.ModelName=="c0000"&&source.CharaInitID==6080) {
            approximate=true;
            limitations.Add("Petrus uses the source unarmed standing idle. His MSB initial animation is -1; active equipment-driven idle selection and source weapon attachments are not resolved.");
        }
        var loaded=Load(readSource,source.ModelName,clipID);var clip=loaded.Animation;
        if(clip.FrameCount>256)throw new NotSupportedException("Source NPC idle exceeds the verified single-block native encoder");
        string path=$"chr/{target}.anibnd.dcx";
        if(!files.TryGetValue(path,out var bytes))throw new FileNotFoundException("Native NPC animation package is unavailable: "+path);
        var bank=BND4.Read(bytes);var skeletons=bank.Files.Where(f=>f.ID==1000000).ToArray();
        var taes=bank.Files.Where(f=>TAE.Is(f.Bytes.ToArray())).ToArray();
        if(skeletons.Length!=1||taes.Length!=1)throw new InvalidDataException("Native NPC animation rig or event bank is ambiguous");
        var rig=NativeAnimation.Skeleton(skeletons[0].Bytes.ToArray());
        if(rig.Names.Any(n=>skin.Model.Nodes.Count(b=>b.Name==n)!=1))throw new InvalidDataException("NPC appearance and native animation skeleton do not match");
        var encoded=NativeAnimation.Convert(clip,skeletons[0].Bytes.ToArray(),skin.Bones,skin.ProportionScale,preserveSourceWorld:true);
        var tae=TAE.Read(taes[0].Bytes);
        foreach(int slot in IdleSlots) {
            var targets=bank.Files.Where(f=>f.ID==1000000000+slot).ToArray();var rows=tae.Animations.Where(a=>a.ID==slot).ToArray();
            string name=$"a{slot/1000000:D3}_000000";
            if(targets.Length!=1||rows.Length!=1||!Leaf(targets[0].Name).Equals(name+".hkx",StringComparison.OrdinalIgnoreCase)||
                !rows[0].AnimFileName.Equals(name+".hkt",StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Native NPC idle playback slots changed: "+slot);
            var old=new NativeAnimation.Pack(targets[0].Bytes.ToArray());float oldDuration=old.Float(old.Object("hkaSplineCompressedAnimation")+16);
            if(oldDuration<=0)throw new InvalidDataException("Invalid native NPC idle duration");
            targets[0].Bytes=encoded;
            var row=rows[0];row.Events.RemoveAll(DollIdleEvent);
            foreach(var e in row.Events) {
                if(!float.IsFinite(e.StartTime)||!float.IsFinite(e.EndTime)||e.StartTime<0||e.EndTime<e.StartTime)
                    throw new InvalidDataException("Invalid native NPC idle event timing");
                e.StartTime=Math.Clamp(e.StartTime/oldDuration,0,1)*clip.Duration;
                e.EndTime=Math.Clamp(e.EndTime/oldDuration,0,1)*clip.Duration;
            }
        }
        taes[0].Bytes=tae.Write();
        var output=bank.Write(bank.Compression);var check=BND4.Read(output);
        if(check.Files.Count!=bank.Files.Count||IdleSlots.Any(slot=>!check.Files.Single(f=>f.ID==1000000000+slot).Bytes.Span.SequenceEqual(encoded)))
            throw new InvalidDataException("NPC idle animation archive failed round-trip verification");
        limitations.Add("Source NPC pose is preserved on a native Bloodborne playback rig. Blended bind-shape remapping, native hitboxes/physics and absent source cloth simulation remain approximations; gameplay pose/loop validation is required.");
        limitations.Add("Source dialogue, AI, weapons and quest-driven pose/availability changes are not ported by this idle stage. Native generic idle state gates remain; only identified Doll idle modifiers, effects and audio are removed.");
        files[path]=output;
        return new(source.ModelName,clipID,loaded.Archive,clip.Duration,clip.FrameCount,Array.AsReadOnly(IdleSlots),approximate,limitations.AsReadOnly());
    }
}
