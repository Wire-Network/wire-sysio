import { EventEmitter } from "events";
import { JsonRpc, RpcError, Api } from "eosjs";
import { JsSignatureProvider } from "eosjs/dist/eosjs-jssig";
import { TextDecoder, TextEncoder } from "text-encoding";
import {
  GetAccountResult,
  GetBlockHeaderStateResult,
  GetBlockResult,
  GetCodeResult,
  GetInfoResult,
  GetKeyAccountsResult,
  GetProducerScheduleResult,
  GetProducersResult,
  GetScheduledTransactionsResult,
  GetTransactionResult,
  PushTransactionArgs,
  ReadOnlyTransactResult,
} from "eosjs/dist/eosjs-rpc-interfaces";
import { Transaction, TransactResult } from "eosjs/dist/eosjs-api-interfaces";
import fetch from "node-fetch";
import { Action } from "eosjs/dist/eosjs-serialize";
​
export class Chain {
    public rpc: JsonRpc;
    public api: Api;
​
    contractName: string = "perks";
    private permission: string = "active";
​
    constructor() {
        // this.init();
        let key = process.env.EOSIO_PRIVATE_KEY;
        let endpoint = process.env.EOSIO_ENDPOINT;
        if(!endpoint || !key) throw new Error("Missing EOSIO_ENDPOINT or EOSIO_PRIVATE_KEY");
        // console.log("Init", key);
​
        let signatureProvider: JsSignatureProvider = new JsSignatureProvider(
            key ? [key] : []
        );
        let rpc: JsonRpc = (this.rpc = new JsonRpc(endpoint, {
            fetch,
        }));

        this.api = new Api({
            rpc,
            signatureProvider,
            textDecoder: new TextDecoder(),
            textEncoder: new TextEncoder(),
        });
    }
​
​
    getRows(options: GetRowsOptions): Promise<GetRowData>;
    getRows<T>(options: GetRowsOptions): Promise<GetRows<T>>;
    getRows(options: GetRowsOptions) {
        return new Promise(async (res, rej) => {
        let defaults: GetRowsOptions = {
            scope: this.contractName,
            code: this.contractName,
            limit: 9999,
            reverse: false,
        };
        Object.entries(defaults).forEach(([key, value]) => {
            if (!options.hasOwnProperty(key))
            options[key as keyof GetRowsOptions] = value;
        });
        // console.log('GET ROWS', options);
​
        try {
            let result = await this.rpc!.get_table_rows({
            json: true,
            code: options.code,
            scope: options.scope ? options.scope : options.code,
            table: options.table,
            index_position: options.index_position,
            limit: options.limit,
            lower_bound: options.lower_bound,
            upper_bound: options.upper_bound,
            key_type: options.key_type,
            reverse: options.reverse,
            });
            // console.log(result);
            res(result);
        } catch (e: any) {
            console.log("\nCaught exception on get_table_rows: ", e);
            if (e instanceof RpcError) rej(JSON.stringify(e.json, null, 2));
        }
        });
    }
​
    pushTransaction(
        transaction: TransactionData | TransactionData[],
        options?: TransactOptions
    ): Promise<PushTransactionArgs | TransactResult | ReadOnlyTransactResult> {
        return new Promise(async (res, rej) => {
        let actions: Action[] = [];
​
        if (Array.isArray(transaction)) {
            for (let option of transaction) {
                let { account, name, actor, data, permission } = option;
                actions.push({
                    account: account ? account : this.contractName,
                    name: name,
                    authorization: [
                    {
                        actor: actor,
                        permission: permission ? permission : this.permission,
                    },
                    ],
                    data: data,
                });
            }
        } else {
            let { account, name, actor, data, permission } = transaction;
            actions.push({
            account: account ? account : this.contractName,
            name: name,
            authorization: [
                {
                actor: actor,
                permission: permission ? permission : this.permission,
                },
            ],
            data: data,
            });
        }
​
        try {
            let transaction: Transaction = { actions };
            if (options) {
            if (options.delay_sec) transaction.delay_sec = options.delay_sec;
            }
​
            // console.log("BEFORE TRANSACT", transaction);
​
            const result = await this.api!.transact(transaction, {
            blocksBehind: 3,
            expireSeconds: 3600,
            });
​
            res(result);
        } catch (e: any) {
            // console.log("\nCaught exception on transact: " + e);
            rej(
            e.toString().replace("Error: assertion failure with message: ", "")
            );
        }
        });
    }
​
    getInfo(): Promise<GetInfoResult> {
        return new Promise(async (resolve, reject) => {
            let info = await this.rpc.get_info().catch(err => {
                reject(err);
            });
            if (!info) return;
            resolve(info);
        });
    }
    
    // param is how many blocks from the latest you want to have
    getLatestBlocks(amount: number): Promise<GetBlockResult[]> {
        return new Promise(async (resolve, reject) => {
            let blocks: GetBlockResult[] = [];
            const info = await this.getInfo().catch(err => reject(err));
            if (!info) return;
            for (let i = 0; i < amount; i++) {
                let block = await this.rpc.get_block(info.head_block_num - i).catch(err => reject(err));
                if (!block) return;
                blocks.push(block);
            }
            resolve(blocks);
        });
    }
​
    /**
     *
     * @param start - The block number to start with (if no end param, just get the block as start as the block num)
     * @param end - If provided, the block number that creates the range
     * @returns Blocks and the last irreversible block number
     */
    getBlocks(
        start: number,
        end?: number
    ): Promise<{ blocks: GetBlockResult[]; lastIrreversible: number }> {
        return new Promise(async (resolve, reject) => {
        let blocks: GetBlockResult[] = [];
        if (end == undefined || start == end) {
            let block = await this.rpc.get_block(start).catch((err) => {
            // console.log("error getting block:", start);
            reject(err);
            });
            if (!block) return;
            blocks.push(block);
        } else {
            let _start = start < end ? start : end;
            let _end = _start == start ? end : start;
            let length = _end - start;
            for (let i = 0; i < length; i++) {
            let block = await this.rpc.get_block(_start + i).catch((err) => {
                // console.log("error getting block: ", _start + i);
                reject(err);
            });
            if (!block) return;
            blocks.push(block);
            }
        }
        const info = await this.getInfo().catch((err) => {
            reject(err);
        });
        if (!info) return;
        resolve({ blocks, lastIrreversible: info.last_irreversible_block_num });
        });
    }
​
    /**
     *
     * @param tx_ids transaction ids to be returned
     * @returns Transactions
     */
    getTxs(tx_ids: string[]): Promise<GetTransactionResult[]> {
        return new Promise(async (resolve, reject) => {
        let txs: GetTransactionResult[] = [];
        for (let id of tx_ids) {
            let tx = await this.rpc.history_get_transaction(id).catch((err) => {
            // console.log("error getting tx:", tx);
            reject(err);
            });
            if (!tx) return;
            txs.push(tx);
        }
        resolve(txs);
        });
    }
​
    getAccounts(account_names: string[]): Promise<GetAccountResult[]> {
        return new Promise(async (resolve, reject) => {
        let accounts: GetAccountResult[] = [];
        for (let name of account_names) {
            let account = await this.rpc.get_account(name).catch((err) => {
            // console.log("error finding account:", name);
            reject(err);
            });
            if (!account) return;
            accounts.push(account);
        }
        resolve(accounts);
        });
    }
​
    getCode(account: string): Promise<GetCodeResult> {
        return new Promise(async (resolve, reject) => {
        let code = await this.rpc.get_code(account).catch((err) => {
            // console.log("error getting code from account:", account);
            reject(err);
        });
        if (!code) return;
        resolve(code);
        });
    }
​
    getProducers(): Promise<GetProducersResult> {
        return new Promise(async (resolve, reject) => {
        let producers = await this.rpc.get_producers(true).catch((err) => {
            // console.log("error getting producers");
            reject(err);
        });
        if (!producers) return;
        resolve(producers);
        });
    }
​
    getProducerSchedule(): Promise<GetProducerScheduleResult> {
        return new Promise(async (resolve, reject) => {
        let schedule = await this.rpc.get_producer_schedule().catch((err) => {
            // console.log("error getting schedule");
            reject(err);
        });
        if (!schedule) return;
        resolve(schedule);
        });
    }
​
    getScheduledTxs(
        lower_bound: string | undefined = undefined,
        limit: number | undefined = undefined
    ): Promise<GetScheduledTransactionsResult> {
        return new Promise(async (resolve, reject) => {
        let scheduledTx = await this.rpc
            .get_scheduled_transactions(true, lower_bound, limit)
            .catch((err) => {
            // console.log("error getting scheduled tx");
            reject(err);
            });
        if (!scheduledTx) return;
        resolve(scheduledTx);
        });
    }
​
    getBlockHeaderState(
        block_num_or_id: string | number
    ): Promise<GetBlockHeaderStateResult> {
        return new Promise(async (resolve, reject) => {
        let bhs = await this.rpc
            .get_block_header_state(block_num_or_id)
            .catch((err) => {
            // console.log(
            //     "error getting the block header state to:",
            //     block_num_or_id
            // );
            reject(err);
            });
        if (!bhs) return;
        resolve(bhs);
        });
    }
​
    getAccountByPubKey(pub_key: string): Promise<GetKeyAccountsResult> {
        return new Promise(async (resolve, reject) => {
        let account = await this.rpc
            .history_get_key_accounts(pub_key)
            .catch((err) => {
            // console.log("error getting the account from the pubkey:", pub_key);
            reject(err);
            });
        if (!account) return;
        resolve(account);
        });
    }
}
​
export interface TransactOptions {
  delay_sec?: number;
  returnFailureTraces?: boolean;
}
​
export interface GetRowsOptions {
  code?: string;                    // contract name
  scope?: string;                   // contract name
  table?: string;                   // table name
  index_position?: string | number; // secondary index position
  limit?: number;                   // max 9999
  lower_bound?: string | number;    // searching for value
  upper_bound?: string | number;    // searching for value
  key_type?: string;                // secondary index type
  reverse?: boolean;                // reverse order
}
​
export interface GetRows<T> {
  rows: Array<T>;
  more: boolean;
  next_key: string;
}
​
export interface GetRowData {
  rows: Array<any>;
  more: boolean;
  next_key: string;
}
​
export interface TransactionData {
  account?: string;
  name: string;
  actor: string;
  permission?: "owner" | "active" | string;
  data: any;
}

const chain = new Chain();
export default chain;