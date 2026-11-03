// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Text.RegularExpressions;

// NPC actions have one TAE and can import physical motions from declared
// split archives. Event imports and HKX-only aliases are different chains.
internal sealed class SourceCharacterActions {
    internal sealed record Action(int ID,int EventID,int MotionID,string Archive,SourceAnimation Clip,IReadOnlyList<TAE.Event> Events);
    readonly Dictionary<int,TAE.Animation> rows=[];
    readonly Dictionary<int,List<(byte[] Bytes,string Archive)>> motions=[];
    readonly Dictionary<int,Action> resolved=[];
    readonly Dictionary<int,SourceAnimation> decoded=[];
    readonly byte[] skeleton;
    readonly Func<string,byte[]?> read;
    readonly string[] splits;
    bool indexed;
    long budget;
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Source character action: "+why);}
    BND3 Bank(string path) {
        var bytes=read(path)??throw new FileNotFoundException("Declared source animation archive unavailable",path);
        Need(bytes.Length is >0 and <=128*1024*1024&&(budget+=bytes.Length)<=512L*1024*1024,"archive read budget");
        var bank=BND3.Read(bytes);Need(bank.Files.Count is >0 and <=16384,"archive member budget");return bank;
    }
    internal SourceCharacterActions(Func<string,byte[]?> read,string model) {
        Need(Regex.IsMatch(model,@"\Ac\d{4}\z"),"invalid model identity");this.read=read;
        string path="chr/"+model+".anibnd.dcx";var main=Bank(path);
        skeleton=main.Files.Single(f=>Leaf(f.Name).Equals("skeleton.hkx",StringComparison.OrdinalIgnoreCase)).Bytes.ToArray();
        var tae=TAE.Read(main.Files.Single(f=>TAE.Is(f.Bytes.ToArray())).Bytes);
        Need(!tae.BigEndian&&tae.Format==TAE.TAEFormat.DS1&&tae.ID==200000+int.Parse(model.AsSpan(1)),"event bank format/identity changed");
        foreach(var row in tae.Animations)Need(row.ID is >=0 and <10000&&rows.TryAdd((int)row.ID,row),"repeated/out-of-range action");
        Need(rows.Count is >0 and <=10000,"invalid action population");
        splits=main.Files.Where(f=>f.ID is >=6000000 and <7000000).Select(f=>Regex.Match(Leaf(f.Name),@"\A(?:load_|delayload_)?("+model+@"_[A-Za-z0-9_]+)\.txt\z",RegexOptions.IgnoreCase))
            .Where(m=>m.Success).Select(m=>"chr/"+m.Groups[1].Value+".anibnd.dcx").Distinct(StringComparer.OrdinalIgnoreCase).Order(StringComparer.Ordinal).ToArray();
        Need(splits.Length<=64,"split declaration budget");Add(main,path);
    }
    void Add(BND3 bank,string path) {
        foreach(var file in bank.Files) {
            var m=Regex.Match(Leaf(file.Name),@"\Aa00_(\d{4})\.hkx\z",RegexOptions.IgnoreCase);if(!m.Success)continue;
            int id=int.Parse(m.Groups[1].Value);Need(file.Bytes.Length is >0 and <=16*1024*1024,"motion read budget");
            if(!motions.TryGetValue(id,out var files))motions.Add(id,files=[]);files.Add((file.Bytes.ToArray(),path));
        }
    }
    (int ID,TAE.Animation Row) Follow(int id,bool motion,HashSet<int> seen) {
        Need(id is >=0 and <10000&&seen.Count<64&&seen.Add(id),"invalid/cyclic action import");Need(rows.TryGetValue(id,out var row),"missing action "+id);
        return row!.MiniHeader switch {
            TAE.Animation.AnimMiniHeader.ImportOtherAnim i=>Follow(i.ImportFromAnimID,motion,seen),
            TAE.Animation.AnimMiniHeader.Standard s when motion&&s.ImportsHKX=>Follow(s.ImportHKXSourceAnimID,true,seen),
            TAE.Animation.AnimMiniHeader.Standard=>(id,row),
            _=>throw new InvalidDataException("Unsupported source character action header")};
    }
    internal bool Contains(int id)=>rows.ContainsKey(id);
    internal Action Resolve(int id) {
        if(resolved.TryGetValue(id,out var existing))return existing;
        var events=Follow(id,false,[]);var motion=Follow(id,true,[]);
        var m=Regex.Match(Leaf(motion.Row.AnimFileName),@"\Aa00_(\d{4})\.(?:hkxwin|hkx)\z",RegexOptions.IgnoreCase);Need(m.Success,"invalid physical motion name");
        int physical=int.Parse(m.Groups[1].Value);
        if(!indexed){foreach(string path in splits)Add(Bank(path),path);indexed=true;}
        Need(motions.TryGetValue(physical,out var found)&&found.Count==1,"missing/ambiguous declared physical motion "+physical);
        if(!decoded.TryGetValue(physical,out var clip)){clip=SourceAnimation.Decode(skeleton,found![0].Bytes);decoded.Add(physical,clip);}
        var result=new Action(id,events.ID,physical,found![0].Archive,clip,events.Row.Events.Select(e=>e.GetClone(false)).ToArray());resolved.Add(id,result);return result;
    }
}
