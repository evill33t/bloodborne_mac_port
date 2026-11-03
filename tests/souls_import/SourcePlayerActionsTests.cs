// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
static class SourcePlayerActionsTests {
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Check(bool b,string why){if(!b)throw new Exception(why);}
    static void Reject(Action a){try{a();}catch(InvalidDataException){return;}catch(FileNotFoundException){return;}throw new Exception("Invalid player action accepted");}
    static void Main(string[] args) {
        string root=Path.GetFullPath(args[0]);var hashes=new Dictionary<string,string>();
        byte[]? Read(string path){string p=Path.Combine(root,path);if(!File.Exists(p))return null;var b=File.ReadAllBytes(p);hashes[p]=Convert.ToHexStringLower(SHA256.HashData(b));return b;}
        var actions=new SourcePlayerActions(Read);var seen=new List<object>();
        foreach(int id in new[]{353000,353001,353002,353030,353300,353301,354000,354001,354002,354030,354300,354301,20000,20100,20101,20200,20201,20300,20500,120000,120100,120101,120200,120201,120300,120500,0,500,1500,2000,2001,2002}) {
            var a=actions.Resolve(id);Check(a.ID==id&&a.Clip.Bones.Count==61&&ReferenceEquals(a,actions.Resolve(id)),"Player action identity/cache/rig changed");
            Check(a.Archive.StartsWith("chr/c0000_",StringComparison.Ordinal)&&a.Archive.EndsWith(".anibnd.dcx",StringComparison.Ordinal),"Player motion escaped declared split archives");
            foreach(float t in new[]{0,a.Clip.Duration/2,a.Clip.Duration}) {
                var p=a.Clip.Sample(t);Check(p.Length==61&&p.All(m=>float.IsFinite(m.GetDeterminant())),"Invalid player animation sample");
            }
            seen.Add(new{id,a.EventID,a.MotionID,a.Filename,a.Archive,a.Clip.Duration,a.Clip.FrameCount});
        }
        Check(actions.Resolve(353002).EventID==353001&&actions.Resolve(353002).MotionID==353001,"Event/motion alias did not follow its authored bank");
        Check(actions.Resolve(353030).MotionID==253030,"Cross-bank import was incorrectly kept in Dragon Tooth bank");
        Check(actions.Resolve(353000).Events.Any(e=>e.Type==1&&BitConverter.ToInt32(e.GetParameterBytes(false),8)==0)&&actions.Resolve(354000).Events.Any(e=>e.Type==1&&BitConverter.ToInt32(e.GetParameterBytes(false),8)==200),"One-/two-hand source contacts were mixed");
        Check(!actions.Contains(-1)&&!actions.Contains(9999999),"Invalid action reported present");Reject(()=>actions.Resolve(-1));Reject(()=>actions.Resolve(9999999));
        var main=BND3.Read(Read("chr/c0000.anibnd.dcx")!);var file=main.Files.Single(f=>Leaf(f.Name)=="a35.tae");var tae=TAE.Read(file.Bytes);
        tae.Animations.Single(a=>a.ID==3002).MiniHeader=new TAE.Animation.AnimMiniHeader.ImportOtherAnim{ImportFromAnimID=353002};file.Bytes=tae.Write();var cycle=main.Write();
        Reject(()=>new SourcePlayerActions(p=>p=="chr/c0000.anibnd.dcx"?cycle:Read(p)).Resolve(353002));
        Reject(()=>new SourcePlayerActions(p=>p=="chr/c0000.anibnd.dcx"?Read(p):null).Resolve(353000));
        foreach(var (path,sha) in hashes)Check(Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(path)))==sha,"Original source animation changed");
        if(args.Length>1){Directory.CreateDirectory(args[1]);File.WriteAllText(Path.Combine(args[1],"validation.json"),JsonSerializer.Serialize(new{actions=seen,hashes},new JsonSerializerOptions{WriteIndented=true}));}
        Console.WriteLine($"PASS: {seen.Count} source player actions, authored cross-bank aliases, one/two-hand contacts, independent samples, invalid/cyclic/missing motion rejection; {hashes.Count} original archives unchanged");
    }
}
