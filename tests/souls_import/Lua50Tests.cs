// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Security.Cryptography;
using System.Text;
static class Lua50Tests {
    static void Check(bool value,string message){if(!value)throw new Exception(message);}
    static void Reject(Action action){try{action();}catch(InvalidDataException){return;}throw new Exception("Malformed or unowned Lua edit accepted");}
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Main(string[] args) {
        var originals=new Dictionary<string,byte[]>();
        byte[] Read(string p){var b=File.ReadAllBytes(p);originals[p]=SHA256.HashData(b);return b;}
        var source=BND3.Read(Read(Path.Combine(args[0],"script/m12_00_00_00.luabnd.dcx")));
        var native=BND4.Read(Read(Path.Combine(args[1],"script/aicommon.luabnd.dcx")));
        var files=source.Files.Concat(native.Files).Where(f=>Leaf(f.Name).EndsWith(".lua",StringComparison.Ordinal)).ToArray();int count=0;
        foreach(var f in files){var input=f.Bytes.ToArray();Check(Lua50.Write(Lua50.Read(input)).AsSpan().SequenceEqual(input),"Lossy bytecode round trip: "+f.Name);count++;}
        var chunk=source.Files.Single(f=>Leaf(f.Name)=="233000_battle.lua").Bytes.ToArray();var before=Lua50.Read(chunk);
        var symbols=Lua50.Globals(before,true).Append("Jujin233000Battle").ToDictionary(n=>n,n=>"DS1_Foliage_"+n,StringComparer.Ordinal);
        var patched=Lua50.Rewrite(chunk,symbols,new Dictionary<string,double>{{"GOAL_Jujin233000_Battle",882500}},new Dictionary<double,double>{{3004,3003}});var after=Lua50.Read(patched);
        Check(Lua50.Globals(after,true).SetEquals(symbols.Values.Where(s=>s!="DS1_Foliage_Jujin233000Battle")),"Source writable globals escaped their owned namespace");
        Check(!Lua50.Globals(after).Contains("GOAL_Jujin233000_Battle")&&Lua50.All(after).Any(f=>f.Constants.Contains(882500d)),"Owned goal was not materialized as a numeric load");
        Check(!Lua50.All(after).Any(f=>f.Constants.Contains(3004d)),"Unimplemented paired grab was retained");
        foreach(var (a,b) in Lua50.All(before).Zip(Lua50.All(after))) {
            Check(a.Code.Length==b.Code.Length&&a.Lines.SequenceEqual(b.Lines)&&a.Upvalues==b.Upvalues&&a.Stack==b.Stack,"Edit changed bytecode control flow or closure ABI");
            foreach(var (x,y) in a.Code.Zip(b.Code))if(x!=y) {
                Check((x&63)==5&&(y&63)==1&&(x&~63u)==(y&~63u),"Edit changed an instruction other than its explicit goal constant load");
                Check(Encoding.UTF8.GetString((byte[])a.Constants[x>>6&262143]!)=="GOAL_Jujin233000_Battle","Wrong global read changed");
            }
        }
        Reject(()=>Lua50.Read(chunk[..^1]));Reject(()=>Lua50.Read([..chunk,0]));var bad=chunk.ToArray();bad[7]=4;Reject(()=>Lua50.Read(bad));
        Reject(()=>Lua50.Rewrite(chunk,new Dictionary<string,string>{{"missing","owned"}},new Dictionary<string,double>()));
        Reject(()=>Lua50.Rewrite(chunk,new Dictionary<string,string>(),new Dictionary<string,double>{{"Jujin233000Battle_Activate",882500}}));
        Reject(()=>Lua50.Rewrite(chunk,new Dictionary<string,string>(),new Dictionary<string,double>(),new Dictionary<double,double>{{99999,1}}));
        byte[]? Source(string p)=>Read(Path.Combine(args[0],p));byte[] Native(string p)=>Read(Path.Combine(args[1],p));
        var owned=SourceEnemyAi.Foliage(native.Write(),Source,Native);var bank=BND4.Read(owned);
        var goal=LUAINFO.Read(bank.Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo",StringComparison.Ordinal)).Bytes).Goals.Single(g=>g.ID==SourceEnemyAi.FoliageGoal);
        Check(goal.Name==SourceEnemyAi.FoliageName&&goal.BattleInterrupt&&!goal.LogicInterrupt,"Source goal registry contract was lost");
        var combined=Lua50.Read(bank.Files.Single(f=>Leaf(f.Name)==SourceEnemyAi.FoliageGoal+"_battle.lua").Bytes.ToArray());
        if(args.Length>2){Directory.CreateDirectory(args[2]);File.WriteAllBytes(Path.Combine(args[2],"882500_battle.lua"),Lua50.Write(combined));}
        Check(combined.Functions.Length==42&&combined.Code.Count(w=>(w&63)==33)==2,"Source helper closure or capture CLOSE instructions missing");
        Check(Lua50.Globals(combined,true).All(n=>n.StartsWith("DS1_Foliage_",StringComparison.Ordinal)),"Imported source helper overrides native globals");
        foreach(var file in native.Files.Where(f=>!Leaf(f.Name).EndsWith(".luainfo",StringComparison.Ordinal)&&!Leaf(f.Name).EndsWith(".luagnl",StringComparison.Ordinal)))
            Check(bank.Files.Single(f=>f.ID==file.ID).Bytes.Span.SequenceEqual(file.Bytes.Span),"Original native AI was changed");
        Reject(()=>SourceEnemyAi.Foliage(owned,Source,Native));
        foreach(var (path,hash) in originals)Check(SHA256.HashData(File.ReadAllBytes(path)).AsSpan().SequenceEqual(hash),"Original AI bank changed");
        Console.WriteLine($"PASS: {count} shipped Lua 5.0 chunks round-trip byte-exactly; owned globals, goal substitution, grab suppression and malformed-input rejection verified");
    }
}
