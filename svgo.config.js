// svgo.config.js
const reorderRefs = {
    name: 'reorderDefs',
    description: 'place defs at the start of the document',
    type: 'visitor',
    active: true,
    fn: () => ({
        element: {
            enter: (node) => {
                let defs = node.children.filter((child) => child.name === 'defs');
                let remainingChildren = node.children.filter((child) => child.name !== 'defs');
                node.children = [...defs, ...remainingChildren];
            },
        },
    }),

};

module.exports = {
    multipass: true, // boolean. false by default
    js2svg: {
        indent: 2, // string with spaces or number of spaces. 4 by default
        pretty: true, // boolean, false by default
    },
    plugins: [
        // set of built-in plugins enabled by default
        'preset-default',
        reorderRefs,
    ],
};