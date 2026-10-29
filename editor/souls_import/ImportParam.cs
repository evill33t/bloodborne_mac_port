// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;

// Preserve native opaque rows, including padding and null row-name offsets.
internal static class ImportParam {
    internal static PARAM Read(Memory<byte> bytes,string type,int size,short version) {
        var p=PARAM.Read(bytes);
        if(p.ParamType!=type||p.DetectedSize!=size||p.ParamdefDataVersion!=version||p.BigEndian||p.Format2D!=PARAM.FormatFlags1.LongDataOffset)
            throw new InvalidDataException("Unsupported native parameter schema: "+type);
        var def=new PARAMDEF{ParamType=type,DataVersion=version,Unicode=true};
        def.Fields.Add(new PARAMDEF.Field(def,PARAMDEF.DefType.dummy8,"payload"){ArrayLength=size});p.ApplyParamdef(def);return p;
    }
    internal static byte[] Payload(PARAM.Row row)=>((byte[])row.Cells.Single().Value).ToArray();
    internal static PARAM.Row Append(PARAM table,PARAM.Row donor,int id,string name,byte[] payload) {
        if(table.Rows.Any(r=>r.ID==id)||payload.Length!=table.DetectedSize)throw new InvalidDataException("Parameter identity or row size conflict");
        var row=new PARAM.Row(donor){ID=id,Name=name};row.Cells.Single().Value=payload.ToArray();
        int at=table.Rows.FindIndex(r=>r.ID>id);if(at<0)table.Rows.Add(row);else table.Rows.Insert(at,row);return row;
    }
    internal static byte[] Write(PARAM table)=>MapCharacters.WriteRows(table);
}
