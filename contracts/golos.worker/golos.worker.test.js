const EOSTest = require("eosio.test");

const eosTest = new EOSTest();
const appName = "app.sample";
const tokenSymbol = "APP";

const delegateAccounts = [];
const memberAccounts = [];

for (let i = 0; i < 21; i++) {
  const code = String.fromCharCode("a".charCodeAt(0) + i);
  delegateAccounts.push(`delegate.${code}`);
  memberAccounts.push(`user.${code}`);
}

const tokenContractPrefix = "/opt/eosio/contracts/eosio.token/eosio.token";
let tokenContract = null;
let contract = null;

const STATE_TSPEC_APP = 1,
  STATE_TSPEC_CREATE = 2,
  STATE_WORK = 3,
  STATE_TSPEC_AUTHOR_REVIEW = 4,
  STATE_DELEGATES_REVIEW = 5,
  STATE_PAYMENT = 6,
  STATE_CLOSED = 7;

beforeEach(async done => {
  console.log("init");
  await eosTest.init();

  await eosTest.newAccount(
    ...memberAccounts,
    ...delegateAccounts,
    "golos.worker",
    "eosio.token",
    appName
  );

  console.log("update contract account permissions");
  await eosTest.api.updateauth({
    account: "golos.worker",
    permission: "active",
    parent: "owner",
    auth: {
      threshold: 1,
      keys: [
        {
          key: "EOS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV",
          weight: 1
        }
      ],
      accounts: [
        {
          permission: { actor: "golos.worker", permission: "eosio.code" },
          weight: 1
        }
      ]
    }
  });

  console.log("deploy a token contract");
  tokenContract = await eosTest.deploy(
    "eosio.token",
    `${tokenContractPrefix}.wasm`,
    `${tokenContractPrefix}.abi`
  );

  console.log("build contract");
  await eosTest.make(".");

  console.log("create app token");
  await tokenContract.create(appName, `1000000000 ${tokenSymbol}`, {
    authorization: "eosio.token"
  });

  console.log("issue tokens");
  for (account of [appName, ...memberAccounts]) {
    await tokenContract.issue(
      account,
      `1000000 ${tokenSymbol}`,
      "initial supply",
      {
        authorization: appName
      }
    );
  }

  console.log("deploy golos.worker contract");
  contract = await eosTest.deploy(
    "golos.worker",
    "golos.worker.wasm",
    "golos.worker.abi"
  );

  done();
}, 300000);

async function dumpState() {
  console.log(
    "proposals:",
    JSON.stringify(
      await eosTest.api.getTableRows(
        true,
        "golos.worker",
        appName,
        "proposals"
      ),
      null,
      4
    ),
    "funds:",
    JSON.stringify(
      await eosTest.api.getTableRows(true, "golos.worker", appName, "funds"),
      null,
      4
    )
  );
}

async function getProposal(proposalId) {
  return (await eosTest.api.getTableRows(
    { json: true,
      code: "golos.worker",
      scope: appName,
      table: "proposals",
      lower_bound: proposalId,
      limit: 1
    })).rows[0];
}

it(
  "1st use case test",
  async done => {
    console.log("create a workers pool");
    await contract.createpool(appName, tokenSymbol, { authorization: appName });

    console.log("Workers pool replenishment");
    await tokenContract.transfer(
      appName,
      "golos.worker",
      `1000 ${tokenSymbol}`,
      appName /* memo */
    );
    await tokenContract.transfer(
      appName,
      "golos.worker",
      `500 ${tokenSymbol}`,
      appName /* memo */
    );
    let funds = await eosTest.api.getTableRows(
      true,
      "golos.worker",
      appName,
      "funds",
      appName
    );
    console.log("funds table:", funds);
    expect(funds.rows.length).not.toEqual(0);
    expect(funds.rows[0].quantity).toEqual(`1500 ${tokenSymbol}`);

    console.log("addpropos");
    const proposals = [
      {
        id: 0,
        title: "Proposal #1",
        text: "Let's create worker's pool",
        user: memberAccounts[0],
        tspec_app_idx: 1
      },
      {
        id: 1,
        title: "Proposal #2",
        text: "Let's create golos fork",
        user: memberAccounts[1],
        tspec_app_idx: 0
      }
    ];

    const comments = [
      { id: 0, user: delegateAccounts[0], text: "Let's do it!" },
      { id: 1, user: delegateAccounts[1], text: "Noooo!" }
    ];

    const tspecs = [
      {
        id: 0,
        author: memberAccounts[2],
        text: "Technical specification #1",
        specification_cost: `100 ${tokenSymbol}`,
        specification_eta: 3600 * 24 * 7,
        development_cost: `200 ${tokenSymbol}`,
        development_eta: 3600 * 24 * 7,
        payments_count: 2,
        worker: memberAccounts[0]
      },
      {
        id: 1,
        author: memberAccounts[3],
        text: "Technical specification #2",
        specification_cost: `500 ${tokenSymbol}`,
        specification_eta: 3600 * 24 * 7,
        development_cost: `900 ${tokenSymbol}`,
        development_eta: 3600 * 24 * 7,
        payments_count: 1,
        worker: memberAccounts[1],
        fund: memberAccounts[3],
        deposit: `1400 ${tokenSymbol}`
      }
    ];

    for (let proposal of proposals) {
      console.log("add proposal:", proposal);
      await contract.addpropos(
        appName,
        proposal.id,
        proposal.user,
        proposal.title,
        proposal.text,
        { authorization: proposal.user }
      );
      expect((await getProposal(proposal.id)).state).toEqual(STATE_TSPEC_APP);

      console.log("edit proposal:", proposal);
      await contract.editpropos(
        appName,
        proposal.id,
        proposal.title,
        proposal.text,
        { authorization: proposal.user }
      );

      let tspec = tspecs[proposal.tspec_app_idx];
      if (tspec.fund) {
        console.log("transfering tokens to the sponsor's fund");
        await tokenContract.transfer(
          tspec.fund,
          "golos.worker",
          tspec.deposit,
          appName,
          { authorization: tspec.fund }
        );
        console.log("use a sponsored fund for proposal");
        await contract.setfund(
          appName,
          proposal.id,
          tspec.fund,
          tspec.deposit,
          { authorization: tspec.author }
        );
      }

      await contract.votepropos(appName, proposal.id, delegateAccounts[0], 1, {
        authorization: delegateAccounts[0]
      });
      await contract.votepropos(appName, proposal.id, delegateAccounts[1], 0, {
        authorization: delegateAccounts[1]
      });

      for (let comment of comments) {
        console.log("addcomment", comment);
        await contract.addcomment(
          appName,
          proposal.id,
          comment.id,
          comment.user,
          comment,
          { authorization: comment.user }
        );
      }

      for (let comment of comments) {
        console.log("editcomment", comment);
        await contract.editcomment(appName, proposal.id, comment.id, comment, {
          authorization: comment.user
        });
      }

      for (let comment of comments) {
        console.log("delcomment", comment);
        await contract.delcomment(appName, proposal.id, comment.id, {
          authorization: comment.user
        });
      }

      for (let tspec of tspecs) {
        console.log("add technical specification application:", tspec);
        await contract.addtspec(
          appName,
          proposal.id,
          tspec.id,
          tspec.author,
          tspec,
          { authorization: tspec.author }
        );
      }

      let commentId = 0;
      console.log("vote for the technical specification application:", tspec);
      for (let i = 0; i < Math.floor(delegateAccounts.length / 2) + 1; i++) {
        await contract.votetspec(
          appName,
          proposal.id,
          tspec.id,
          delegateAccounts[i],
          1,
          commentId++,
          { text: "I agree" },
          { authorization: delegateAccounts[i] }
        );
      }
      expect((await getProposal(proposal.id)).state).toEqual(
        STATE_TSPEC_CREATE
      );

      console.log("publish a final technical specification");
      await contract.publishtspec(appName, proposal.id, tspec, {
        authorization: tspec.author
      });

      console.log("start work on the features");
      await contract.startwork(appName, proposal.id, tspec.worker, {
        authorization: tspec.author
      });
      expect((await getProposal(proposal.id)).state).toEqual(STATE_WORK);

      console.log("post worker status");
      await contract.poststatus(
        appName,
        proposal.id,
        commentId++,
        { text: "Work in progress #1" },
        { authorization: tspec.worker }
      );

      console.log("finish work on the proposal");
      await contract.poststatus(
        appName,
        proposal.id,
        commentId++,
        { text: "I finished all tasks" },
        { authorization: tspec.worker }
      );
      expect((await getProposal(proposal.id)).state).toEqual(
        STATE_TSPEC_AUTHOR_REVIEW
      );

      console.log("accept work by the technical specification author");
      await contract.acceptwork(
        appName,
        proposal.id,
        commentId++,
        { text: "All work done well" },
        { authorization: tspec.author }
      );
      expect((await getProposal(proposal.id)).state).toEqual(
        STATE_DELEGATES_REVIEW
      );

      console.log("review work by the delegates");
      for (let i = 0; i < delegateAccounts.length; i++) {
        let status = (i + 1) % 2; /* 0 - reject , 1 - accept */
        await contract.reviewwork(
          appName,
          proposal.id,
          delegateAccounts[i],
          status,
          commentId++,
          { text: 'Lorem ipsum dolor sit am' },
          { authorization: delegateAccounts[i] }
        );
      }
      expect((await getProposal(proposal.id)).state).toEqual(STATE_PAYMENT);

      console.log("withdraw work reward");
      do {
        await contract.withdraw(appName, proposal.id, {
          authorization: tspec.worker
        });
      } while (
        (await getProposal(proposal.id)).worker_payments_count !==
        tspec.payments_count
      );

      console.log("check proposal state");
      expect((await getProposal(proposal.id)).state).toEqual(STATE_CLOSED);
    }

    done();
  },
  600000
);

afterEach(async done => {
  await dumpState();
  await eosTest.destroy();
  done();
});
