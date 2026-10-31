// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Buffers.Binary;
using System.Text;

// Additional maps borrow format/behavior templates, never a live retail scene.
internal static class MapDestination {
    internal const string UrnSeed="__import_urn_template";
    internal sealed record Selection(bool Additional,string TargetMap,string TemplateMap) {
        internal string BaseMap=>TargetMap[..6]+"_00_00";
        internal string TemplateBase=>TemplateMap[..6]+"_00_00";
        internal string Area=>TargetMap[..3];
    }
    static string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();
    internal static Selection Select(string mode,string profile,string source,string target) {
        if(mode=="additional") {
            if(profile!="DS1R"||!new[]{("m10_02_00_00","m40_02_00_00"),("m10_01_00_00","m40_03_00_00")}.Contains((source,target)))
                throw new NotSupportedException("Additional destinations currently support DS1R Firelink m40_02 and Undead Burg m40_03");
            return new(true,target,source=="m10_02_00_00"?"m24_01_00_01":"m24_00_00_00");
        }
        if(mode!="replacement"||target is not("m24_01_00_01" or "m24_00_00_00"))throw new NotSupportedException("Unsupported map destination");
        return new(false,target,target);
    }
    internal static MSBB Create(MSBB donor,Selection selection) {
        if(!selection.Additional)throw new ArgumentException("Fresh scenes require an additional destination");
        var result=new MSBB();
        var player=(MSBB.Part.Player)donor.Parts.Players.First().DeepCopy();player.EntityID=-1;result.Parts.Players.Add(player);
        if(player.ModelName!=null)result.Models.Add(donor.Models.GetEntries().Single(m=>m.Name==player.ModelName).DeepCopy());
        var col=(MSBB.Part.Collision)donor.Parts.Collisions.First().DeepCopy();col.EntityID=-1;result.Parts.Collisions.Add(col);
        var model=(MSBB.Model.Collision)donor.Models.Collisions.Single(m=>m.Name==col.ModelName).DeepCopy();model.SibPath=model.SibPath.Replace(selection.TemplateBase,selection.BaseMap,StringComparison.Ordinal);result.Models.Collisions.Add(model);
        // The breakable converter borrows actor fields from this record. It is
        // removed after conversion and is never registered in the shipped scene.
        var urn=donor.Parts.Objects.FirstOrDefault(p=>p.ModelName=="o240028")??donor.Parts.Objects.FirstOrDefault(p=>p.ModelName=="o240035");
        if(urn!=null){var seed=(MSBB.Part.Object)urn.DeepCopy();seed.Name=UrnSeed;seed.EntityID=-1;result.Parts.Objects.Add(seed);}
        return result;
    }
    internal static byte[] EmptyEvents() {
        var script=new EMEVD(EMEVD.Game.Bloodborne);script.Events.Add(new(0));script.Events.Add(new(50));return script.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
    internal static byte[] AppendLoadList(byte[]? original,string target) {
        var bytes=original??[];var text=Encoding.Latin1.GetString(bytes);string entry="map:/MapStudio/"+target+".msb";
        if(text.Split(['\r','\n'],StringSplitOptions.RemoveEmptyEntries).Any(line=>line.Split('#')[0].Trim()==entry))return bytes.ToArray();
        return [..bytes,..Encoding.ASCII.GetBytes((bytes.Length>0&&bytes[^1]!=10?"\r\n":"")+entry+"\r\n")];
    }
    internal static void Seed(Selection selection,Func<string,byte[]?> read,Action<string,byte[]> write) {
        if(!selection.Additional)return;
        byte[] Required(string path)=>read(path)??throw new FileNotFoundException("Destination template unavailable",path);
        write("event/"+selection.BaseMap+".emevd.dcx",EmptyEvents());
        string areaEvents="event/"+selection.Area+".emevd.dcx";
        write(areaEvents,read(areaEvents)??EmptyEvents());
        write($"map/{selection.BaseMap}/{selection.BaseMap}.arealoadlist",AppendLoadList(null,selection.BaseMap));
        string areaList=$"map/{selection.Area}/{selection.Area}.arealoadlist";write(areaList,AppendLoadList(read(areaList),selection.BaseMap));
        foreach(string kind in new[]{".luabnd.dcx",".talkesdbnd.dcx"}) {
            string folder=kind.StartsWith(".talk",StringComparison.Ordinal)?"script/talk/":"script/";
            var binder=BND4.Read(Required(folder+selection.TemplateBase+kind));
            foreach(var file in binder.Files)file.Name=file.Name.Replace(selection.TemplateBase,selection.BaseMap,StringComparison.Ordinal);
            write(folder+selection.BaseMap+kind,binder.Write(DCX.Type.DCX_DFLT_10000_44_9));
        }
        foreach(string suffix in new[]{"_envmap.tpf.dcx","_cgrading.tpf.dcx"}) {
            string donorArea=selection.TemplateMap[..3],path=$"map/{selection.Area}/{selection.Area}{suffix}";
            var tpf=TPF.Read(Required($"map/{donorArea}/{donorArea}{suffix}"));
            foreach(var texture in tpf.Textures)texture.Name=texture.Name.Replace(donorArea+"_",selection.Area+"_",StringComparison.Ordinal);
            var bytes=tpf.Write(DCX.Type.DCX_DFLT_10000_44_9);var existing=read(path);
            if(existing!=null&&!existing.AsSpan().SequenceEqual(bytes))throw new InvalidDataException("Additional area environment texture conflicts: "+path);
            write(path,bytes);
        }
    }
    internal static BXF4 TextureArchive(Selection selection,Func<string,byte[]?> read) {
        string path=$"map/{selection.Area}/{selection.Area}_0003.tpf";var header=read(path+"bhd");var body=read(path+"bdt");
        if((header==null)!=(body==null))throw new InvalidDataException("Additional area texture archive is incomplete");
        if(header!=null)return BXF4.Read(header,body!);
        if(!selection.Additional)throw new FileNotFoundException("Native texture archive unavailable",path);
        string donor=selection.TemplateMap[..3];var result=BXF4.Read(read($"map/{donor}/{donor}_0003.tpfbhd")??throw new FileNotFoundException("Texture header template unavailable"),read($"map/{donor}/{donor}_0003.tpfbdt")??throw new FileNotFoundException("Texture data template unavailable"));result.Files.Clear();return result;
    }
    internal static void Finish(MSBB map,Selection selection,Func<string,byte[]?> read,Action<string,byte[]> write) {
        if(!selection.Additional)return;
        // A connected block can otherwise be resident with only one physical
        // layer, leaving its walkable floor absent. Broaden collision residency
        // while its block is loaded. Keep authored Display groups: maximizing
        // those also exposes distant-view geometry over the detailed map.
        // Events, enable flags, geometry and all other group arrays stay intact.
        foreach(var collision in map.Parts.Collisions)Array.Fill(collision.DrawGroups,uint.MaxValue);
        map.Parts.Objects.RemoveAll(p=>p.Name==UrnSeed);
        var used=map.Parts.GetEntries().Select(p=>p.ModelName).ToHashSet(StringComparer.Ordinal);
        map.Models.MapPieces.RemoveAll(m=>!used.Contains(m.Name));map.Models.Objects.RemoveAll(m=>!used.Contains(m.Name));map.Models.Enemies.RemoveAll(m=>!used.Contains(m.Name));map.Models.Collisions.RemoveAll(m=>!used.Contains(m.Name));
        string talkPath="script/talk/"+selection.BaseMap+".talkesdbnd.dcx";
        var binder=BND4.Read(read(talkPath)??throw new FileNotFoundException("Additional talk binder unavailable"));
        var talks=map.Parts.Enemies.Where(p=>p.TalkID>=0).Select(p=>$"t{p.TalkID}.esd").ToHashSet(StringComparer.Ordinal);
        binder.Files.RemoveAll(f=>!talks.Contains(Leaf(f.Name)));write(talkPath,binder.Write(DCX.Type.DCX_DFLT_10000_44_9));
        _=MSBB.Read(map.Write());
    }
    internal static void VerifyNativeEntityReservations(string nativeRoot,MSBB map) {
        var owned=map.Parts.GetEntries().Select(p=>p.EntityID).Concat(map.Regions.GetEntries().Select(p=>p.EntityID)).Concat(map.Events.GetEntries().Select(p=>p.EntityID)).Where(id=>id>=0).ToHashSet();
        foreach(string path in Directory.EnumerateFiles(Path.Combine(nativeRoot,"map/MapStudio"),"*.msb*",SearchOption.AllDirectories)) {
            var compressed=File.ReadAllBytes(path);var bytes=DCX.Is(compressed)?DCX.Decompress(compressed).ToArray():compressed;
            bool possible=false;for(int at=0;at<=bytes.Length-4;at+=4)if(owned.Contains(BinaryPrimitives.ReadInt32LittleEndian(bytes.AsSpan(at,4)))){possible=true;break;}
            if(!possible)continue;
            var native=MSBB.Read(bytes);var nativeIDs=native.Parts.GetEntries().Select(p=>p.EntityID).Concat(native.Regions.GetEntries().Select(p=>p.EntityID)).Concat(native.Events.GetEntries().Select(p=>p.EntityID));
            foreach(int id in nativeIDs)if(owned.Contains(id))throw new InvalidDataException($"Additional map entity ID is occupied by native data: {id} ({path})");
        }
    }
}
