import { APIClient } from "@wireio/sdk-core"
import { AuthLink } from "../types"

export async function checkLinkStatus(
  api: APIClient,
  contractAccount: string,
  username: string
): Promise<AuthLink | null> {
  if (!username || username.startsWith("0x")) return null

  const response = await api.v1.chain.get_table_rows({
    json: true,
    code: contractAccount,
    scope: contractAccount,
    table: "links",
    index_position: "tertiary" as any,
    limit: 50,
    lower_bound: username as any,
    upper_bound: username as any,
    key_type: "name" as any
  })

  if (response.rows.length > 0) {
    return response.rows[0] as unknown as AuthLink
  }
  return null
}
