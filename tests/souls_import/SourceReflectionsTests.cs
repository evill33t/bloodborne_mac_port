// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;using System.Security.Cryptography;using System.Text.Json;
internal static class SourceReflectionsTests {
    static void Check(bool value,string reason){if(!value)throw new InvalidDataException(reason);}
    static void Main(string[] args) {
        if(args.Length is <2 or >3)throw new ArgumentException("DS1R root and BB dvdroot required");
        var hashes=new Dictionary<string,string>();byte[] Read(string path){var bytes=File.ReadAllBytes(path);hashes.Add(path,Convert.ToHexString(SHA256.HashData(bytes)));return bytes;}
        var source=MSB1.Read(Read(Path.Combine(args[0],"map/MapStudio/m12_00_00_00.msb")));
        string nativePath=Path.Combine(args[1],"map/MapStudio/m24_00_00_00.msb.dcx");if(!File.Exists(nativePath))nativePath=nativePath[..^4];
        var native=MSBB.Read(Read(nativePath));var before=native.Write(DCX.Type.None);
        var sourceParts=source.Parts.GetEntries();var nativeParts=native.Parts.GetEntries();
        Check(sourceParts.Count==598&&nativeParts.Count>=sourceParts.Count&&native.Parts.Collisions.Count>=source.Parts.Collisions.Count,"Installed fixture map inventory changed");
        var originalFlags=nativeParts.Select(p=>(p.DrawByReflectCam,p.DrawOnlyReflectCam)).ToArray();var originalPlanes=native.Parts.Collisions.Select(p=>p.ReflectPlaneHeight).ToArray();
        for(int i=0;i<sourceParts.Count;i++)SourceReflections.CopyFlags(sourceParts[i],nativeParts[i]);
        for(int i=0;i<source.Parts.Collisions.Count;i++)SourceReflections.CopyPlane(source.Parts.Collisions[i],native.Parts.Collisions[i],20.07f);
        var round=MSBB.Read(native.Write(DCX.Type.None));var roundParts=round.Parts.GetEntries();
        for(int i=0;i<sourceParts.Count;i++)Check(roundParts[i].DrawByReflectCam==(sourceParts[i].DrawByReflectCam==1)&&roundParts[i].DrawOnlyReflectCam==(sourceParts[i].DrawOnlyReflectCam==1),"Authored reflection flags did not survive native MSB serialization");
        for(int i=0;i<source.Parts.Collisions.Count;i++)Check(round.Parts.Collisions[i].ReflectPlaneHeight==source.Parts.Collisions[i].ReflectPlaneHeight+20.07f,"Authored source plane lost shared import offset");
        var basin=source.Parts.Collisions.Where(p=>p.ReflectPlaneHeight==-71.74f).ToArray();Check(basin.Length==12,"Authored Basin plane census changed");
        foreach(var col in basin){var target=new MSBB.Part.Collision();SourceReflections.CopyPlane(col,target,20.07f);Check(Math.Abs(target.ReflectPlaneHeight+51.67f)<.00001f,"Basin plane was replaced by donor/mesh height");}
        Check(source.Parts.MapPieces.Where(p=>p.ModelName is "m1710B0" or "m1720B0" or "m1730B0" or "m1740B0" or "m1750B0" or "m1760B0" or "m1770B0" or "m1780B0" or "m1790B0").All(p=>p.DrawByReflectCam==1&&p.DrawOnlyReflectCam==0),"Basin scenery reflection visibility changed");
        // Restore only the three intended fields in the parsed output. Exact
        // canonical native bytes prove all other scene/actor/event data stayed.
        for(int i=0;i<roundParts.Count;i++){roundParts[i].DrawByReflectCam=originalFlags[i].DrawByReflectCam;roundParts[i].DrawOnlyReflectCam=originalFlags[i].DrawOnlyReflectCam;}
        for(int i=0;i<round.Parts.Collisions.Count;i++)round.Parts.Collisions[i].ReflectPlaneHeight=originalPlanes[i];
        Check(round.Write(DCX.Type.None).SequenceEqual(before),"Reflection transfer changed unrelated native MSB fields");
        int negatives=0;void Reject(Action action){var b=round.Write(DCX.Type.None);bool rejected=false;try{action();}catch(InvalidDataException){rejected=true;}Check(rejected&&round.Write(DCX.Type.None).SequenceEqual(b),"Invalid source reflection input mutated output");negatives++;}
        var bad=(MSB1.Part.MapPiece)source.Parts.MapPieces.First().DeepCopy();bad.DrawByReflectCam=2;Reject(()=>SourceReflections.CopyFlags(bad,roundParts[0]));bad.DrawByReflectCam=1;bad.DrawOnlyReflectCam=255;Reject(()=>SourceReflections.CopyFlags(bad,roundParts[0]));
        var badPlane=(MSB1.Part.Collision)source.Parts.Collisions.First().DeepCopy();badPlane.ReflectPlaneHeight=float.NaN;Reject(()=>SourceReflections.CopyPlane(badPlane,round.Parts.Collisions[0],20.07f));badPlane.ReflectPlaneHeight=-71.74f;Reject(()=>SourceReflections.CopyPlane(badPlane,round.Parts.Collisions[0],float.PositiveInfinity));badPlane.ReflectPlaneHeight=1000000;Reject(()=>SourceReflections.CopyPlane(badPlane,round.Parts.Collisions[0],20.07f));
        foreach(var(p,h)in hashes)Check(Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(p)))==h,"Original asset changed");
        string output=args.Length==3?args[2]:"out/map-editor/basin-water/reflection-audit/tests";Directory.CreateDirectory(output);
        File.WriteAllText(Path.Combine(output,"report.json"),JsonSerializer.Serialize(new{sourceParts=sourceParts.Count,sourceCollisions=source.Parts.Collisions.Count,nativeParts=nativeParts.Count,sourceReflectedScenery=source.Parts.MapPieces.Count(p=>p.DrawByReflectCam==1),basinSourcePlane=-71.74f,offsetY=20.07f,basinNativePlane=-71.74f+20.07f,basinCollisions=basin.Select(p=>p.Name),negatives,onlyIntendedFieldsChanged=true,hashes},new JsonSerializerOptions{WriteIndented=true}));
        Console.WriteLine($"PASS: {sourceParts.Count} actual source flags and {source.Parts.Collisions.Count} authored planes survive native MSB roundtrip; only intended fields changed; {negatives} invalid inputs rejected; {hashes.Count} original assets unchanged. Planar-reflection appearance requires native gameplay QA.");
    }
}
