// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Text.RegularExpressions;

// DS1 player-style NPC actions live in numbered TAEs and split HKX binders.
// Event imports and motion-only imports are distinct chains. Follow both by
// their authored global IDs; a35 is Dragon Tooth in either hand, not a36.
internal sealed class SourcePlayerActions {
    internal sealed record Action(int ID,int EventID,int MotionID,string Filename,string Archive,
        SourceAnimation Clip,IReadOnlyList<TAE.Event> Events);
    readonly Dictionary<int,TAE.Animation> actions=[];
    readonly Dictionary<string,List<(byte[] Bytes,string Archive)>> motions=new(StringComparer.OrdinalIgnoreCase);
    readonly Dictionary<int,Action> resolved=[];
    readonly Dictionary<int,SourceAnimation> decoded=[];
    readonly Func<string,byte[]?> read;
    readonly byte[] skeleton;
    readonly string[] splitArchives;
    bool indexed;
    long bytesRead;
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Require(bool b,string why){if(!b)throw new InvalidDataException("Source player action: "+why);}
    BND3 Bank(string path) {
        var bytes=read(path)??throw new FileNotFoundException("Declared source player animation archive unavailable",path);
        Require(bytes.Length is >0 and <=128*1024*1024&&(bytesRead+=bytes.Length)<=512L*1024*1024,"archive read budget exceeded");
        var bank=BND3.Read(bytes);Require(bank.Files.Count<=16384,"archive member budget exceeded");return bank;
    }
    internal SourcePlayerActions(Func<string,byte[]?> source) {
        read=source;var bank=Bank("chr/c0000.anibnd.dcx");
        skeleton=bank.Files.Single(f=>Leaf(f.Name).Equals("skeleton.hkx",StringComparison.OrdinalIgnoreCase)).Bytes.ToArray();
        foreach(var file in bank.Files.Where(f=>TAE.Is(f.Bytes.ToArray()))) {
            var match=Regex.Match(Leaf(file.Name),@"\Aa(\d{2,3})\.tae\z",RegexOptions.IgnoreCase);
            Require(match.Success,"invalid player event-bank name");int number=int.Parse(match.Groups[1].Value);
            var tae=TAE.Read(file.Bytes);Require(!tae.BigEndian&&tae.Format==TAE.TAEFormat.DS1&&tae.ID==2000+number,"unsupported player event bank");
            foreach(var a in tae.Animations)Require(a.ID is >=0 and <10000&&actions.TryAdd(checked(number*10000+(int)a.ID),a),"repeated/out-of-range player action");
        }
        Require(actions.Count is >0 and <=65536,"invalid player action population");
        splitArchives=bank.Files.Where(f=>f.ID is >=6000000 and <7000000).Select(f=>Leaf(f.Name))
            .Where(n=>Regex.IsMatch(n,@"\Ac0000_[A-Za-z0-9_]+\.txt\z",RegexOptions.IgnoreCase))
            .Select(n=>"chr/"+n[..^4]+".anibnd.dcx").Distinct(StringComparer.OrdinalIgnoreCase).Order(StringComparer.Ordinal).ToArray();
        Require(splitArchives.Length is >0 and <=64,"invalid player split archive declarations");
        AddMotions(bank,"chr/c0000.anibnd.dcx");
    }
    void AddMotions(BND3 bank,string path) {
        foreach(var f in bank.Files.Where(f=>Regex.IsMatch(Leaf(f.Name),@"\Aa\d{2,3}_\d{4}\.hkx\z",RegexOptions.IgnoreCase))) {
            string name=Leaf(f.Name);if(!motions.TryGetValue(name,out var entries))motions.Add(name,entries=[]);
            Require(f.Bytes.Length is >0 and <=16*1024*1024,"player motion budget exceeded");entries.Add((f.Bytes.ToArray(),path));
        }
    }
    (int ID,TAE.Animation Row) Follow(int id,bool motion,HashSet<int> seen) {
        Require(id is >=0 and <10000000&&seen.Count<64&&seen.Add(id),"cyclic or invalid player action import");
        Require(actions.TryGetValue(id,out var row),"missing player action "+id);
        return row!.MiniHeader switch {
            TAE.Animation.AnimMiniHeader.ImportOtherAnim i=>Follow(i.ImportFromAnimID,motion,seen),
            TAE.Animation.AnimMiniHeader.Standard s when motion&&s.ImportsHKX=>Follow(s.ImportHKXSourceAnimID,true,seen),
            TAE.Animation.AnimMiniHeader.Standard=>(id,row),
            _=>throw new InvalidDataException("Unsupported source player miniheader")
        };
    }
    internal bool Contains(int id)=>actions.ContainsKey(id);
    internal Action Resolve(int id) {
        if(resolved.TryGetValue(id,out var old))return old;
        var events=Follow(id,false,[]);var motion=Follow(id,true,[]);
        var match=Regex.Match(Leaf(motion.Row.AnimFileName),@"\Aa(\d{2,3})_(\d{4})\.(?:HKXwin|hkx)\z",RegexOptions.IgnoreCase);
        Require(match.Success,"player action has no physical motion: "+id);
        int physical=checked(int.Parse(match.Groups[1].Value)*10000+int.Parse(match.Groups[2].Value));
        string name=$"a{physical/10000:D2}_{physical%10000:D4}.hkx";
        if(!indexed){foreach(string path in splitArchives)AddMotions(Bank(path),path);indexed=true;}
        Require(motions.TryGetValue(name,out var found)&&found.Count==1,"missing/ambiguous declared player motion "+name);
        var input=found![0];
        if(!decoded.TryGetValue(physical,out var clip)){clip=SourceAnimation.Decode(skeleton,input.Bytes);decoded.Add(physical,clip);}
        var result=new Action(id,events.ID,physical,name,input.Archive,clip,events.Row.Events.Select(e=>e.GetClone(false)).ToArray());
        resolved.Add(id,result);return result;
    }
}
