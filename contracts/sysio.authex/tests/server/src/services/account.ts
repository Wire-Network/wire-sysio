import { APIClient, API } from "@wireio/sdk-core"

export async function getAccount(
  api: APIClient,
  name: string
): Promise<API.v1.AccountObject | null> {
  try {
    return await api.v1.chain.get_account(name)
  } catch (err) {
    console.error(err)
    return null
  }
}
