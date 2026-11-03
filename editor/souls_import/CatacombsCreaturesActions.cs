// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Text.RegularExpressions;

// A read-only virtual source bank selects the actor's authored weapon family.
// This avoids treating bow13000 / great-curved-sword23000 as scimitar3000.
internal sealed class CatacombsCreaturesActions {
    readonly BND3 bank;readonly Dictionary<int,TAE.Animation> rows;readonly Dictionary<string,BinderFile> physical;readonly Dictionary<int,int> actions=[];readonly HashSet<int> active;readonly IReadOnlyDictionary<int,int>? extraSlots;
    internal IReadOnlyDictionary<int,int> Selected=>actions;
    internal string Model {get;}
    internal byte[] VirtualBank {get;}
    static string Leaf(string p)=>Path.GetFileName(p.Replace('\\','/'));
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Catacombs actions: "+why);}
    internal CatacombsCreaturesActions(Func<string,byte[]?> read,string model,int offset,IReadOnlySet<int>? activeActions=null,IReadOnlyDictionary<int,int>? extraSlotMap=null) {
        Need(model is "c2900" or "c2910" or "c2930" or "c2650" or "c3501" or "c2300" or "c2794" or "c3300","unsupported source model");Need(offset is 0 or 10000 or 20000,"unsupported source animation family");Model=model;extraSlots=extraSlotMap;
        active=activeActions?.ToHashSet()??((model,offset) switch {("c2650",_)=>[3000],("c2930",_)=>[3000,3001],("c2910",_)=>Enumerable.Range(3000,7).ToHashSet(),("c2900",10000)=>[3000],("c2900",20000)=>[3000,3001,3002,3003,3004,3005,3008],_=>Enumerable.Range(3000,9).ToHashSet()});
        bank=BND3.Read(read("chr/"+model+".anibnd.dcx")??throw new FileNotFoundException("Source animation bank unavailable"));
        var tae=TAE.Read(bank.Files.Single(f=>TAE.Is(f.Bytes.ToArray())).Bytes);Need(!tae.BigEndian&&tae.Format==TAE.TAEFormat.DS1,"source TAE contract changed");
        rows=tae.Animations.ToDictionary(a=>checked((int)a.ID));physical=bank.Files.ToDictionary(f=>Leaf(f.Name),f=>f.Clone(),StringComparer.OrdinalIgnoreCase);
        // SourceAnimation's bounded public reader accepts a00 only. Virtual
        // copies change neither source disk data nor the source events; each
        // selected absolute action addresses its actual authored HKX bytes.
        foreach(int id in rows.Keys.Where(i=>i is >=0 and <10000).Order()) {
            if(id is >=2300 and <=2433)continue;int selected=id;
            if(offset>0&&id is >=3000 and <4000&&rows.ContainsKey(offset+id))selected=offset+id;
            var from=Motion(selected,[]);string name=$"a00_{id:D4}.hkx";
            var existing=bank.Files.SingleOrDefault(f=>Leaf(f.Name).Equals(name,StringComparison.OrdinalIgnoreCase));
            if(existing==null){existing=from.Clone();existing.ID=bank.Files.Max(f=>f.ID)+1;existing.Name=Regex.Replace(from.Name,@"a\d\d_\d{4}\.hkx$",name,RegexOptions.IgnoreCase);bank.Files.Add(existing);}
            existing.Bytes=from.Bytes.ToArray();actions.Add(id,selected);
        }
        Need(actions.ContainsKey(0)&&actions.ContainsKey(3000),"source idle/attack unavailable");VirtualBank=bank.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
    BinderFile Motion(int id,HashSet<int> seen) {
        Need(seen.Add(id)&&seen.Count<64&&rows.TryGetValue(id,out _),"invalid source alias");var row=rows[id];
        if(row.MiniHeader is TAE.Animation.AnimMiniHeader.ImportOtherAnim i)return Motion(i.ImportFromAnimID,seen);
        if(row.MiniHeader is TAE.Animation.AnimMiniHeader.Standard s&&s.ImportsHKX)return Motion(s.ImportHKXSourceAnimID,seen);
        var m=Regex.Match(Leaf(row.AnimFileName),@"\Aa(\d\d)_(\d{4})\.(?:hkxwin|hkx)\z",RegexOptions.IgnoreCase);Need(m.Success,"invalid source physical name");
        string name=$"a{m.Groups[1].Value}_{m.Groups[2].Value}.hkx";Need(physical.TryGetValue(name,out _),"source motion is not in the declared main bank: "+name);return physical[name];
    }
    internal int SourceSlot(int native) {
        int id=native%1000000;if(extraSlots?.TryGetValue(id,out int extra)==true){Need(actions.ContainsKey(extra),"declared extra playback absent");return extra;}if(id is >=2300 and <=2433)return 0;if(id is >=3000 and <4000&&!active.Contains(id))return 3000;
        if(Model is "c2900" or "c2910"&&id==7004)return 9001;
        if(Model is "c2900" or "c2910"&&id==7005)return 9061;
        if(Model=="c2900"&&id==7006)return 9000;
        if(Model=="c2900"&&id==7007)return 9060;
        if(actions.ContainsKey(id))return id;
        if(id is >=3000 and <4000)return 3000;
        int fallback=id switch {501 or 502 or 503=>500,700=>701,1510 or 1520 or 1521=>1500,1610 or 1620 or 1621=>2200,
            1751 or 1760 or 1770 or 1771=>1750,2003=>2004,2060=>2014,2100 or 2110=>2004,
            >=2600 and <=2623=>new[]{2000,2001,2002,2004}[id%10%4],_=>0};
        return actions.ContainsKey(fallback)?fallback:0;
    }

    internal IReadOnlyList<TAE.Event> Events(int nativeSlot) {
        int slot=SourceSlot(nativeSlot);int id=actions.GetValueOrDefault(slot,slot);var seen=new HashSet<int>();
        while(rows[id].MiniHeader is TAE.Animation.AnimMiniHeader.ImportOtherAnim alias){Need(seen.Add(id),"cyclic source event alias");id=alias.ImportFromAnimID;}
        return rows[id].Events.Select(e=>e.GetClone(false)).ToArray();
    }
    internal Func<string,byte[]?> Reader(Func<string,byte[]?> source)=>path=>path=="chr/"+Model+".anibnd.dcx"?VirtualBank:source(path);
}
