// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Security.Cryptography;
using System.Text;
static class NativePlayerAnimationBindingsTests {
    static void Check(bool b,string why){if(!b)throw new Exception(why);}
    static void Reject(Action a){try{a();}catch(InvalidDataException){return;}throw new Exception("Invalid graph binding accepted");}
    // Independent section/fixup reader. Do not validate an appended graph
    // solely through the production writer's own interpretation.
    sealed class Raw {
        internal readonly byte[] Bytes;internal readonly int Data,Size;
        internal readonly Dictionary<int,int> Local=[];internal readonly Dictionary<int,int> Global=[];
        internal readonly Dictionary<int,(int Section,int Name,string Type)> Objects=[];
        internal Raw(byte[] bytes) {
            Bytes=bytes;int section=64+BitConverter.ToUInt16(bytes,62),header=section+128,classes=BitConverter.ToInt32(bytes,section+20);
            Data=BitConverter.ToInt32(bytes,header+20);Size=BitConverter.ToInt32(bytes,header+24);int global=BitConverter.ToInt32(bytes,header+28),virt=BitConverter.ToInt32(bytes,header+32),end=BitConverter.ToInt32(bytes,header+36);
            Check(Size>0&&global>=Size&&virt>=global&&end>=virt&&Data+end<=bytes.Length,"Invalid exported fixup section bounds");
            for(int p=Size;p+8<=global;p+=8)if(Int(p)!=-1)Local.Add(Int(p),Int(p+4));
            for(int p=global;p+12<=virt;p+=12)if(Int(p)!=-1){Check(Int(p+4)==2,"Unexpected external object link");Global.Add(Int(p),Int(p+8));}
            for(int p=virt;p+12<=end;p+=12)if(Int(p)!=-1){Check(Int(p+4)==0,"Unexpected external class name");int at=classes+Int(p+8),stop=Array.IndexOf(bytes,(byte)0,at);Objects.Add(Int(p),(0,Int(p+8),Encoding.UTF8.GetString(bytes,at,stop-at)));}
            foreach(int p in Local.Keys.Concat(Global.Keys))Check(p>=0&&p<=Size-8&&BitConverter.ToUInt64(bytes,Data+p)==0,"Relocated/bad pointer slot serialized");
        }
        internal int Int(int at)=>BitConverter.ToInt32(Bytes,Data+at);
        internal string Text(int field){int at=Data+Local[field],end=Array.IndexOf(Bytes,(byte)0,at);return Encoding.UTF8.GetString(Bytes,at,end-at);}
        internal int[] Children(int field)=>Enumerable.Range(0,Int(field+8)).Select(i=>Global[Local[field]+i*8]).ToArray();
        internal string[] Names(int field)=>Enumerable.Range(0,Int(field+8)).Select(i=>Text(Local[field]+i*8)).ToArray();
    }
    static void Main(string[] args) {
        string path=Path.Combine(args[0],"chr/c0000.anibnd.dcx");var original=File.ReadAllBytes(path);string hash=Convert.ToHexStringLower(SHA256.HashData(original));
        var bank=BND4.Read(original);var graph=bank.Files.Single(f=>f.ID==7000500).Bytes.ToArray();var character=bank.Files.Single(f=>f.ID==7000400).Bytes.ToArray();
        NativePlayerAnimationBindings.Bank[] mappings=[new(105,185,new HashSet<int>{13,14}),new(0,85,new HashSet<int>{11}),new(22,86,new HashSet<int>{11})];
        var result=NativePlayerAnimationBindings.Append(graph,character,mappings);Check(result.Selectors==1675&&result.Clips==1838&&result.AnimationNames.Count==1430,"Private player bank binding population changed");
        var before=new Raw(graph);var after=new Raw(result.Graph);var oldCharacter=new Raw(character);var newCharacter=new Raw(result.Character);
        Check(before.Objects.Count==12970&&after.Objects.Count==16483,"Clip/fallback virtual fixups are missing");
        Check(before.Global.All(p=>after.Global[p.Key]==p.Value)&&before.Objects.All(p=>after.Objects[p.Key]==p.Value),"An original native object reference/class changed");
        var changed=new HashSet<int>();int selectors=0,owned=0;
        foreach(var o in before.Objects.Where(p=>p.Value.Type=="CustomManualSelectorGenerator")) {
            int field=o.Key+136;var old=before.Children(field);var current=after.Children(field);
            if(before.Local[field]==after.Local[field]){Check(old.SequenceEqual(current),"Unselected selector children changed");continue;}
            selectors++;changed.Add(field);Check(current.Take(old.Length).SequenceEqual(old)&&current.Length>old.Length+1,"Original selector indexes/order changed");
            for(int i=o.Key+144;i<o.Key+152;i++)changed.Add(i);
            int fallback=current[^1];Check(fallback!=old[^1]&&after.Objects[fallback].Type=="hkbClipGenerator"&&after.Text(fallback+144)==before.Text(old[^1]+144),"Native no-match fallback animation changed or leaf state was shared");
            Check(after.Text(fallback+56).StartsWith(before.Text(old[^1]+56),StringComparison.Ordinal),"Fallback node prefix changed");
            Check(after.Bytes.AsSpan(after.Data+fallback,352).SequenceEqual(before.Bytes.AsSpan(before.Data+old[^1],352)),"Fallback clip control bytes changed");
            foreach(int child in current.Skip(old.Length).SkipLast(1)) {
                Check(after.Objects[child].Type=="hkbClipGenerator","Owned child has no native virtual class");string animation=after.Text(child+144);int from=animation.StartsWith("a185_",StringComparison.Ordinal)?105:animation.StartsWith("a085_",StringComparison.Ordinal)?0:animation.StartsWith("a086_",StringComparison.Ordinal)?22:-1;
                Check(from>=0,"Owned child escaped its bank reservation");string source=$"a{from:D3}"+animation[4..];int donor=old.Single(p=>before.Text(p+144)==source);
                Check(after.Text(child+56).StartsWith(animation,StringComparison.Ordinal)&&after.Bytes.AsSpan(after.Data+child,352).SequenceEqual(before.Bytes.AsSpan(before.Data+donor,352)),"Owned clip changed controls, source identity or name matching");
                foreach(var p in before.Global.Where(p=>p.Key>=donor&&p.Key<donor+352))Check(after.Global[child+p.Key-donor]==p.Value,"Owned clip lost its native trigger/binding link");owned++;
            }
        }
        Check(selectors==1675&&owned==1838&&before.Local.All(p=>changed.Contains(p.Key)||after.Local[p.Key]==p.Value),"Unrelated native local pointer changed");
        for(int i=0;i<before.Size;i++)if(!changed.Contains(i))Check(graph[before.Data+i]==result.Graph[after.Data+i],"Original graph data byte changed outside owned array count/capacity");
        const int nameField=2792;var oldNames=oldCharacter.Names(nameField);var newNames=newCharacter.Names(nameField);
        Check(oldNames.Length==6426&&newNames.Length==7856&&newNames.Take(oldNames.Length).SequenceEqual(oldNames)&&newNames.Skip(oldNames.Length).ToHashSet().SetEquals(result.AnimationNames),"Original Character name indexes changed or owned names missing");
        Check(oldCharacter.Global.All(p=>newCharacter.Global[p.Key]==p.Value)&&oldCharacter.Objects.All(p=>newCharacter.Objects[p.Key]==p.Value)&&oldCharacter.Local.All(p=>p.Key==nameField||newCharacter.Local[p.Key]==p.Value),"Character unrelated references changed");
        for(int i=0;i<oldCharacter.Size;i++)if(i is <2800 or >=2808)Check(character[oldCharacter.Data+i]==result.Character[newCharacter.Data+i],"Character data changed outside name array count/capacity");
        Reject(()=>NativePlayerAnimationBindings.Append(result.Graph,result.Character,mappings));Reject(()=>NativePlayerAnimationBindings.Append(graph,character,[new(105,105,new HashSet<int>{13})]));
        var bad=graph.ToArray();bad[^1]^=1;Reject(()=>NativePlayerAnimationBindings.Append(bad,character,mappings));
        Check(Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(path)))==hash,"Original player animation archive changed");
        if(args.Length>1){Directory.CreateDirectory(args[1]);File.WriteAllBytes(Path.Combine(args[1],"owned-graph.hkx"),result.Graph);File.WriteAllBytes(Path.Combine(args[1],"owned-character.hkx"),result.Character);}
        Console.WriteLine("PASS:1675 private selectors,1838 clip leaves,1675 independent native fallback leaves,1430 Character names; original indexes/controls/fixups preserved. Structural binding check only; no gameplay claim.");
    }
}
