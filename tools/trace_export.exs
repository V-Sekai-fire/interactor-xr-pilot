# SPDX-License-Identifier: Apache-2.0 OR MIT
#
# Exports a trace store's views as delivery files. The store keeps each view as a bare JPEG XL
# codestream with no metadata (the database carries it); a delivered file must carry its own. This
# wraps each codestream, byte for byte, in a JPEG XL container (ISO/IEC 18181-2) with an XMP box:
#
#   from CITATION.cff   dc:title, dc:creator, dc:rights and cc:license (SPDX), dc:relation, dc:identifier
#   from the store      dc:source (store, session and span UUIDs, role), dc:description (the tool
#                       and its arguments), xmp:CreateDate (the span's begin for view_before, its
#                       end for view_after), xmpMM:DocumentID and InstanceID (urn:uuid: of the view)
#
# Each file is named by its view's UUIDv7.
#
# The pixels are not re-encoded. Works on both schema-5 stores (cua-tracer and XR Pilot).
#
#   elixir tools/trace_export.exs <traces.sqlite> <out dir> [CITATION.cff]
#
# Every CFF field is optional: what the file has goes into the XMP, what it lacks is left out, and
# without a CITATION.cff the XMP carries the store's fields only.
#
# Needs the sqlite3 command (3.33 or newer, for -json).

Mix.install([{:yaml_elixir, "~> 2.11"}])

defmodule TraceExport do
  @signature <<0, 0, 0, 12, "JXL ", 0x0D, 0x0A, 0x87, 0x0A>>
  @ftyp <<0, 0, 0, 20, "ftyp", "jxl ", 0::32, "jxl ">>

  def main([store, out]), do: main([store, out, nil])

  def main([store, out, cff]) do
    with {:ok, citation} <- citation(cff),
         {:ok, rows} <- views(store),
         :ok <- File.mkdir_p(out) do
      results = Enum.map(rows, &export(&1, citation, store, out))
      written = Enum.filter(results, &match?({:ok, _}, &1))
      failed = Enum.filter(results, &match?({:error, _}, &1))
      Enum.each(failed, fn {:error, reason} -> IO.puts(:stderr, "not exported: #{inspect(reason)}") end)
      IO.puts("exported #{length(written)} of #{length(rows)} views to #{out}")
      if failed == [], do: :ok, else: {:error, {:views_not_exported, length(failed)}}
    end
  end

  def main(_), do: {:error, :usage}

  # The CFF fields a delivered image carries, each only when the file has it.
  def citation(nil), do: {:ok, %{}}

  def citation(path) do
    case YamlElixir.read_from_file(path) do
      {:ok, doc} when is_map(doc) ->
        {:ok,
         %{
           title: doc["title"],
           creators: Enum.map(List.wrap(doc["authors"]), &author/1),
           license: doc["license"],
           relation: doc["repository-code"] || doc["url"],
           identifier: doc["doi"]
         }}

      {:ok, other} ->
        {:error, {:cff_not_a_mapping, other}}

      {:error, reason} ->
        {:error, {:cff_unreadable, path, reason}}
    end
  end

  defp author(%{"name" => name}), do: name
  defp author(a), do: String.trim("#{a["given-names"]} #{a["family-names"]}")

  # Every JPEG XL view of a decider example, with what its XMP needs.
  def views(store) do
    sql = """
    SELECT se.uuid AS session_uuid, se.started_at, s.uuid AS span_uuid, r.name AS role, s.started,
           (SELECT ended FROM span_end WHERE span_id = s.span_id) AS ended,
           t.name || COALESCE(' ' || d.detail, '') AS state, a.uuid AS asset_uuid, hex(a.bytes) AS hex
    FROM decider_input i
    JOIN input_role r USING (input_role_id)
    JOIN asset a ON a.asset_id = i.asset_id
    JOIN asset_kind k ON k.asset_kind_id = a.asset_kind_id AND k.name = 'jxl'
    JOIN span s ON s.span_id = i.span_id
    JOIN session se ON se.session_id = s.session_id
    JOIN tool t ON t.tool_id = s.tool_id
    LEFT JOIN span_detail d ON d.span_id = s.span_id
    ORDER BY s.uuid, r.name
    """

    case System.find_executable("sqlite3") do
      nil ->
        {:error, {:not_found, "sqlite3"}}

      exe ->
        case System.cmd(exe, ["-readonly", "-json", store, sql], stderr_to_stdout: true) do
          {"", 0} -> {:ok, []}
          {json, 0} -> {:ok, :json.decode(json)}
          {message, code} -> {:error, {:sqlite3, code, String.trim(message)}}
        end
    end
  end

  def export(row, citation, store, out) do
    codestream = Base.decode16!(row["hex"])
    at = if row["role"] == "view_after", do: row["ended"], else: row["started"]

    with <<0xFF, 0x0A, _::binary>> <- codestream,
         {:ok, created} <- civil_plus(row["started_at"], at) do
      xmp = xmp(citation, row, Path.basename(store), created)
      name = "#{row["asset_uuid"]}.jxl"
      path = Path.join(out, name)

      with {:ok, xml} <- box("xml ", xmp),
           {:ok, jxlc} <- box("jxlc", codestream),
           :ok <- File.write(path, [@signature, @ftyp, xml, jxlc]) do
        {:ok, path}
      end
    else
      {:error, _} = error -> error
      _ -> {:error, {:not_a_jxl_codestream, row["span_uuid"], row["role"]}}
    end
  end

  defp box(type, payload) when byte_size(payload) + 8 <= 0xFFFFFFFF,
    do: {:ok, [<<byte_size(payload) + 8::32>>, type, payload]}

  defp box(type, _), do: {:error, {:box_too_large, type}}

  # The session's civil start plus a span offset (`PT1M2.5S`), as ISO 8601 UTC.
  def civil_plus(_started_at, nil), do: {:ok, nil}

  def civil_plus(started_at, offset) do
    with {:ok, start, 0} <- DateTime.from_iso8601(started_at),
         {:ok, ms} <- duration_ms(offset) do
      {:ok, start |> DateTime.add(ms, :millisecond) |> DateTime.to_iso8601()}
    else
      _ -> {:error, {:bad_time, started_at, offset}}
    end
  end

  def duration_ms(text) do
    case Regex.run(~r/^PT(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)(?:\.(\d{1,3}))?S)?$/, text) do
      nil ->
        {:error, {:bad_duration, text}}

      [_ | parts] ->
        [h, m, s, frac] = parts ++ List.duplicate("", 4 - length(parts))
        ms = String.pad_trailing(frac, 3, "0")
        {:ok, ((int(h) * 60 + int(m)) * 60 + int(s)) * 1000 + int(ms)}
    end
  end

  defp int(""), do: 0
  defp int(digits), do: String.to_integer(digits)

  def xmp(c, row, store, created) do
    source = "#{store} session #{row["session_uuid"]} span #{row["span_uuid"]} #{row["role"]}"
    creators = Enum.map_join(Map.get(c, :creators, []), "", &"<rdf:li>#{esc(&1)}</rdf:li>")

    fields =
      [
        "<dc:format>image/jxl</dc:format>",
        c[:title] && "<dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">#{esc(c.title)}</rdf:li></rdf:Alt></dc:title>",
        creators != "" && "<dc:creator><rdf:Seq>#{creators}</rdf:Seq></dc:creator>",
        c[:license] && "<dc:rights><rdf:Alt><rdf:li xml:lang=\"x-default\">#{esc(c.license)}</rdf:li></rdf:Alt></dc:rights>",
        c[:license] && "<cc:license rdf:resource=\"https://spdx.org/licenses/#{esc(c.license)}.html\"/>",
        "<dc:source>#{esc(source)}</dc:source>",
        "<dc:description><rdf:Alt><rdf:li xml:lang=\"x-default\">#{esc(row["state"])}</rdf:li></rdf:Alt></dc:description>",
        c[:relation] && "<dc:relation><rdf:Bag><rdf:li>#{esc(c.relation)}</rdf:li></rdf:Bag></dc:relation>",
        c[:identifier] && "<dc:identifier>doi:#{esc(c.identifier)}</dc:identifier>",
        "<xmpMM:DocumentID>urn:uuid:#{row["asset_uuid"]}</xmpMM:DocumentID>",
        "<xmpMM:InstanceID>urn:uuid:#{row["asset_uuid"]}</xmpMM:InstanceID>",
        created && "<xmp:CreateDate>#{created}</xmp:CreateDate>"
      ]
      |> Enum.filter(&is_binary/1)
      |> Enum.join()

    """
    <?xpacket begin="\uFEFF" id="W5M0MpCehiHzreSzNTczkc9d"?>\
    <x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">\
    <rdf:Description rdf:about="" xmlns:dc="http://purl.org/dc/elements/1.1/" \
    xmlns:xmp="http://ns.adobe.com/xap/1.0/" xmlns:xmpMM="http://ns.adobe.com/xap/1.0/mm/" \
    xmlns:cc="http://creativecommons.org/ns#">\
    #{fields}\
    </rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end="r"?>\
    """
  end

  defp esc(text) do
    text
    |> to_string()
    |> String.replace("&", "&amp;")
    |> String.replace("<", "&lt;")
    |> String.replace(">", "&gt;")
    |> String.replace("\"", "&quot;")
  end
end

case TraceExport.main(System.argv()) do
  :ok ->
    :ok

  {:error, :usage} ->
    IO.puts(:stderr, "usage: elixir tools/trace_export.exs <traces.sqlite> <out dir> [CITATION.cff]")
    System.halt(2)

  {:error, reason} ->
    IO.puts(:stderr, "export failed: #{inspect(reason)}")
    System.halt(1)
end
