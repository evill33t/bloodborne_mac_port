// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Text.Json;
using System.Text.RegularExpressions;

internal static partial class Program {
    static void CharacterPreview(string output,string profile,string model,Mesh preview,string notice){
        if(preview.Positions.Length==0 || preview.Positions.Length>8000000 || preview.Indices.Length==0 || preview.Indices.Length>24000000)throw new InvalidDataException("Character preview exceeds the geometry budget or has no geometry");
        using var file=new FileStream(output,FileMode.CreateNew,FileAccess.Write);
        try{using var writer=new BinaryWriter(file,System.Text.Encoding.UTF8,true);
            writer.Write(System.Text.Encoding.ASCII.GetBytes("BBMSRC01"));Text(writer,profile);Text(writer,"m00_00_00_00");Text(writer,notice);writer.Write(1);Text(writer,model);writer.Write(preview.Positions.Length);writer.Write(preview.Indices.Length);writer.Write(preview.Color);foreach(var p in preview.Positions)Vector(writer,p);foreach(var i in preview.Indices)writer.Write(i);
            writer.Write(1);Text(writer,model+"_0000");Text(writer,model);writer.Write(2u);writer.Write(-1);Vector(writer,System.Numerics.Vector3.Zero);Vector(writer,System.Numerics.Vector3.Zero);Vector(writer,System.Numerics.Vector3.One);
        }catch{file.Dispose();File.Delete(output);throw;}
    }
    static int PreviewCharacter(Dictionary<string,string> values,Profile profile,string root){
        var model=values.GetValueOrDefault("--model","");if(!Regex.IsMatch(model,@"\Ac\d{4,5}\z"))throw new ArgumentException("Expected a character model ID such as c2660");
        var output=Canonical(values.GetValueOrDefault("--output","") is {Length:>0} path?path:throw new ArgumentException("Missing --output"));
        if(output.Equals(root,StringComparison.OrdinalIgnoreCase)||output.StartsWith(root.TrimEnd(Path.DirectorySeparatorChar)+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase))throw new IOException("Output must be outside the source game folder");
        using var assets=new Assets(root,profile.Id);var data=FindModel(assets,profile.Id,"m00_00_00_00",model)??throw new FileNotFoundException("Source character model unavailable");var mesh=Decode(data,model);
        if(mesh.Positions.Length==0 || mesh.Indices.Length==0)throw new InvalidDataException("Source character contains no drawable geometry (some humanoids use separate equipment parts)");
        CharacterPreview(output,profile.Id,model,mesh,"Untextured source character in bind pose; animations, AI and collision are not previewed");Console.WriteLine("PREVIEWED CHARACTER "+profile.Id+" "+model+" -> "+output);return 0;
    }
    static int BakeSkin(Dictionary<string,string> values,Profile profile,string root){
        string Get(string key)=>values.TryGetValue(key,out var value)?value:throw new ArgumentException("Missing "+key);
        var model=Get("--model");var template=Get("--template");if(!Regex.IsMatch(model,@"\Ac\d{4,5}\z") || !Regex.IsMatch(template,@"\Ac\d{4,5}\z"))throw new ArgumentException("Expected character model IDs such as c1430 and c2630");
        var destination=values.GetValueOrDefault("--target",template);if(!Regex.IsMatch(destination,@"\Ac\d{4}\z"))throw new ArgumentException("Destination character must have four digits");
        var native=Canonical(Get("--native-root"));var output=Canonical(Get("--output"));foreach(var game in new[]{root,native})if(output.Equals(game,StringComparison.OrdinalIgnoreCase)||output.StartsWith(game.TrimEnd(Path.DirectorySeparatorChar)+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase))throw new IOException("Output must be outside both source game folders");
        if(Directory.Exists(output) || File.Exists(output))throw new IOException("Choose a new output folder");
        using var assets=new Assets(root,profile.Id);var character=assets.Read($"chr/{model}.chrbnd.dcx")??assets.Read($"chr/{model}.chrbnd")??assets.Read($"model/chr/{model}.bnd")??throw new FileNotFoundException("Source character binder unavailable");
        List<BinderFile> Binder(byte[] b){var raw=Unpack(b).ToArray();return BND4.Is(raw)?BND4.Read(raw).Files:BND3.Is(raw)?BND3.Read(raw).Files:throw new InvalidDataException("Unsupported source binder");}
        var files=Binder(character);var sourceModel=files.FirstOrDefault(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase)||Leaf(f.Name).EndsWith(".flv",StringComparison.OrdinalIgnoreCase))??throw new InvalidDataException("Source binder has no FLVER");
        var textures=new List<TPF.Texture>();foreach(var file in files)if(TPF.Is(file.Bytes.ToArray()))textures.AddRange(TPF.Read(file.Bytes).Textures);
        if((assets.Read($"chr/{model}.texbnd.dcx")??assets.Read($"model/chr/{model}.texbnd")) is {} tex)foreach(var file in Binder(tex))if(TPF.Is(file.Bytes.ToArray()))textures.AddRange(TPF.Read(file.Bytes).Textures);
        foreach(var suffix in new[]{"_2.tpf.dcx",".tpf.dcx",".tpf"})if(assets.Read($"chr/{model}{suffix}") is {} bytes)textures.AddRange(TPF.Read(bytes).Textures);
        var target=BND4.Read(File.ReadAllBytes(Path.Combine(native,"dvdroot_ps4/chr",template+".chrbnd.dcx")));var targetModel=target.Files.Single(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase));
        var masks=Get("--mask").Split(',').Select(int.Parse).ToHashSet();if(masks.Any(i=>i<0 || i>31))throw new ArgumentException("Render masks must be between 0 and 31");
        var bake=SkinRetarget.Bake(sourceModel.Bytes,targetModel.Bytes,textures,masks,textureOwner:destination);targetModel.Bytes=bake.Model.Write();
        // Cloth bindings address the template's old vertex buffers. Converted meshes use
        // ordinary skinning, so retaining those bindings would write into unrelated meshes.
        target.Files.RemoveAll(f=>Leaf(f.Name).EndsWith("_c.hkx",StringComparison.OrdinalIgnoreCase)||Leaf(f.Name).EndsWith(".clm2",StringComparison.OrdinalIgnoreCase)||Leaf(f.Name).Equals("div_data.txt",StringComparison.OrdinalIgnoreCase));
        // Character textures are resolved from the raw TPF inside this binder. An
        // outer DCX envelope here is interpreted as a platform header by the game.
        target.Files.RemoveAll(f=>TPF.Is(f.Bytes.ToArray()));
        if(target.Files.Any(f=>f.ID==100))throw new InvalidDataException("Native template reserves texture binder ID 100");
        target.Files.Add(new BinderFile(SoulsFormats.Binder.FileFlags.Flag1,100,@"N:\SPRJ\data\INTERROOT_ps4\chr\"+destination+@"\"+destination+".tpf",bake.Textures.Write(DCX.Type.None)));
        foreach(var file in target.Files)file.Name=file.Name.Replace(template,destination,StringComparison.OrdinalIgnoreCase);
        target.Files.Sort((a,b)=>a.ID.CompareTo(b.ID));
        var characterBytes=target.Write(DCX.Type.DCX_DFLT_10000_44_9);
        var package=new Dictionary<string,byte[]>{{"chr/"+destination+".chrbnd.dcx",characterBytes}};
        if(destination!=template){
            foreach(var suffix in new[]{".chrbnd.dcx",".anibnd.dcx"})if(File.Exists(Path.Combine(native,"dvdroot_ps4/chr",destination+suffix)))throw new IOException("Destination model already exists in Bloodborne");
            if(File.Exists(Path.Combine(native,"dvdroot_ps4/action/script",destination+".hks")))throw new IOException("Destination character script already exists in Bloodborne");
            var animation=BND4.Read(File.ReadAllBytes(Path.Combine(native,"dvdroot_ps4/chr",template+".anibnd.dcx")));
            foreach(var file in animation.Files){file.Name=file.Name.Replace(template,destination,StringComparison.OrdinalIgnoreCase);if(TAE.Is(file.Bytes.ToArray())){var tae=TAE.Read(file.Bytes);tae.ID=200000+int.Parse(destination[1..]);file.Bytes=tae.Write();}else if(file.ID>=7000000 && file.ID<8000000)file.Bytes=SkinRetarget.RenameBehavior(file.Bytes,template,destination);}
            package["chr/"+destination+".anibnd.dcx"]=animation.Write(DCX.Type.DCX_DFLT_10000_44_9);
            package["action/script/"+destination+".hks"]=File.ReadAllBytes(Path.Combine(native,"dvdroot_ps4/action/script",template+".hks"));
        }
        var stage=Directory.CreateTempSubdirectory("bbmap-skin-");
        try{
            foreach(var (relative,data) in package){var path=Path.Combine(stage.FullName,"dvdroot_ps4",relative);Directory.CreateDirectory(Path.GetDirectoryName(path)!);File.WriteAllBytes(path,data);}
            CharacterPreview(Path.Combine(stage.FullName,"preview.bbsource"),profile.Id,destination,Decode(bake.Model.Write(),destination),"Converted character in Bloodborne bind pose");
            File.WriteAllText(Path.Combine(stage.FullName,"conversion.json"),JsonSerializer.Serialize(new{source=profile.Id,model,template,destination,masks,bake.Vertices,bake.Triangles,bake.DirectWeightShare,bake.Bones,bake.TextureFallbacks,textures=bake.Textures.Textures.Count,behavior="Native Bloodborne template animation, AI and hitbox assets; foreign cloth is skinned without cloth simulation"},new JsonSerializerOptions{WriteIndented=true}));
            File.WriteAllText(Path.Combine(stage.FullName,"README.txt"),$"Converted {profile.Id} {model} geometry and textures onto Bloodborne {template}. "+(destination==template?$"This skin test replaces every instance of {template}. ":$"Independent model {destination}. ")+"Uses native template animations, AI and hitboxes. Original game files remain unchanged.\n");Directory.CreateDirectory(Path.GetDirectoryName(output)!);Directory.Move(stage.FullName,output);
        }catch{if(stage.Exists)stage.Delete(true);throw;}
        Console.WriteLine($"BAKED SKIN {profile.Id} {model} -> {template}: {bake.Vertices} vertices, {bake.Triangles} triangles, {bake.Textures.Textures.Count} PS4 textures, {bake.DirectWeightShare:P1} direct skin weight -> {output}");return 0;
    }
}
